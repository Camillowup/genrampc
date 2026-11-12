// GENRA MELODY - the melody companion of GENRA CHOP, on its own MPC track (monophonic).
//
// SPDX-License-Identifier: GPL-3.0-only
//
// Pages (genra-chop/tools/build_skin.py), on the function keys:
//   MELODY the melody of the loop with GENRA CHOP's chords, NEW, UNDO, AUTO | PADS, KEY LOCK, RHYTHM
//          (Chop / Free / Sparse), NOTES (Scale / Chord / Penta), PATTERN (DENSITY, SWING, RANGE, OCTAVE),
//          TONE (CUTOFF, ATTACK, DECAY, LEVEL)
//   SOUND  the kinds KEYS / RHODES / PLUCK / LEAD / BELL / FLUTE, their presets to tap, and the sound:
//          OSC (TONE, BITE, DETUNE, AGE), FILTER (CUTOFF, RESO, ENV, DRIVE), ENVELOPE (ATTACK, DECAY, SUSTAIN,
//          RELEASE): one Q-Link row each
//   PRINT  the melody as a MIDI file; afterwards PADS
//   SETUP  the status
//
// The line comes from GENRA CHOP over LINK v3 (common/genra_link.h): the engine's melody, which this plugin shapes:
// RHYTHM Chop puts the notes on GENRA CHOP's chops, Free keeps the engine's rhythm, Sparse keeps only the main notes
// and lets them ring; NOTES Scale keeps every note in the key, Chord pulls it onto a chord tone, Penta onto the
// pentatonic scale; RANGE folds the line into one to three octaves; DENSITY thins it out. Accents and slides of the
// engine stay (slides glide, accents play louder and harder). AUTO plays it on the MPC's transport; PADS plays the
// notes of this track (KEY LOCK pulls them into GENRA CHOP's key). NEW / UNDO ask GENRA CHOP for a new melody / the
// last one.
//
// Sound: one voice, the engine is TONE:
//   E-Piano (2-operator FM, a tine on the attack), Wurli (FM through a reed-like saturation), Clav (a bright plucked
//   string), Vibes (sine with a decaying 4th partial), String (plucked string, Karplus-Strong), Kalimba (sine with an
//   inharmonic partial), Square, Saw (PolyBLEP, a second detuned oscillator), Brass (two saws), Bell (FM 1:3.5),
//   Glass (sine with a 2.76 partial), Flute (sine, breath noise, vibrato that comes in late).
//   BITE = how hard the attack is (FM index, tine, breath), DETUNE = second oscillator / chorus, AGE = tape: wow,
//   flutter, hiss, dull top, above 60 % fewer bits. VIBRATO (presets only) = vibrato on leads and flutes, tremolo on keys.
// -> low-pass (SVF, key tracking) -> DRIVE -> ADSR -> level compensation (a closed filter is lifted up to +9 dB)
// -> soft limiter.
// No "ui": the filter envelope always opens at once and only falls; the more ENV, the less resonance.
// Threads: process() (audio); a worker copies the loop out of the shared memory, builds the line and the screen.

#include "mpc_plugin.h"
#include "dsp/oscillator.h"
#include "dsp/svf.h"
#include "../../common/genra_files.h"
#include "../../common/genra_link.h"
#include "../../common/genra_ui.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

using namespace mpc;
using namespace genrafiles;

namespace {

static const int kSteps = genralink::kSteps;
static const int kCats = 6, kPresetRows = 6, kPrintRows = 7;
static const int kColFrames = 34; // melody column: 0 empty, 1..16 a note at that height, +17 the step that plays

const char* const kNotes[12] = {"C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};
const char* const kQual[8] = {"", "m", "7", "maj7", "m7", "m9", "dim", "sus4"};
const int kQualTones[8][5] = {{0, 4, 7, -1, -1}, {0, 3, 7, -1, -1}, {0, 4, 7, 10, -1}, {0, 4, 7, 11, -1},
                              {0, 3, 7, 10, -1}, {0, 3, 7, 10, 2}, {0, 3, 6, -1, -1}, {0, 5, 7, -1, -1}};
const int kMinor[7] = {0, 2, 3, 5, 7, 8, 10};
const int kMajor[7] = {0, 2, 4, 5, 7, 9, 11};
const int kPentaMinor[5] = {0, 3, 5, 7, 10};
const int kPentaMajor[5] = {0, 2, 4, 7, 9};
const char* const kCatNames[kCats] = {"KEYS", "RHODES", "PLUCK", "LEAD", "BELL", "FLUTE"};
enum Engine { E_EPIANO, E_WURLI, E_CLAV, E_VIBES, E_STRING, E_KALIMBA, E_SQUARE, E_SAW, E_BRASS, E_BELL, E_GLASS,
              E_FLUTE, NUM_ENGINES };
const char* const kEngineNames[NUM_ENGINES] = {"E-Piano", "Wurli", "Clav", "Vibes", "String", "Kalimba", "Square",
                                               "Saw", "Brass", "Bell", "Glass", "Flute"};

// a preset: tone, bite, detune, age, cutoff, reso, env, drive, attack, decay, sustain, release, glide, vibrato
static const int kSoundN = 14;
struct Preset { const char* name; float v[kSoundN]; };
const Preset kPresets[kCats][kPresetRows] = {
    {{"Dusty Wurli", {E_WURLI, 45, 6, 35, 62, 10, 20, 25, 1, 55, 40, 30, 10, 20}},
     {"Soul Clav", {E_CLAV, 70, 0, 10, 70, 15, 25, 30, 0, 40, 0, 15, 0, 0}},
     {"Felt Vibes", {E_VIBES, 30, 0, 15, 55, 5, 10, 5, 1, 70, 0, 50, 0, 35}},
     {"Basement Keys", {E_EPIANO, 40, 8, 55, 48, 5, 15, 20, 1, 60, 35, 35, 10, 15}},
     {"Warm Wurli", {E_WURLI, 30, 4, 15, 52, 5, 10, 15, 2, 65, 45, 35, 10, 30}},
     {"Muted Clav", {E_CLAV, 50, 0, 20, 45, 10, 20, 20, 0, 30, 0, 10, 0, 0}}},
    {{"Dilla Rhodes", {E_EPIANO, 35, 10, 45, 45, 5, 15, 15, 1, 65, 45, 40, 10, 15}},
     {"Tape Suitcase", {E_EPIANO, 50, 6, 60, 55, 5, 15, 20, 1, 60, 40, 40, 10, 45}},
     {"Bitcrushed Tine", {E_EPIANO, 70, 4, 80, 60, 10, 20, 45, 0, 50, 30, 30, 5, 10}},
     {"Soul Rhodes", {E_EPIANO, 55, 6, 20, 62, 5, 20, 20, 1, 65, 45, 40, 10, 25}},
     {"Late Night Tine", {E_EPIANO, 40, 12, 35, 40, 5, 10, 10, 2, 75, 50, 50, 15, 30}},
     {"Dark Stage", {E_EPIANO, 60, 5, 25, 35, 10, 25, 35, 1, 60, 40, 35, 10, 10}}},
    {{"Koto Chop", {E_STRING, 80, 0, 15, 70, 10, 15, 20, 0, 55, 0, 30, 0, 0}},
     {"Muted Guitar", {E_STRING, 40, 0, 20, 40, 5, 10, 15, 0, 30, 0, 15, 0, 0}},
     {"Kalimba Dust", {E_KALIMBA, 55, 0, 50, 58, 5, 10, 10, 0, 50, 0, 35, 0, 0}},
     {"Harp Pluck", {E_STRING, 60, 0, 10, 62, 5, 10, 5, 0, 75, 0, 50, 0, 0}},
     {"Thumb Piano", {E_KALIMBA, 75, 0, 25, 65, 5, 15, 15, 0, 40, 0, 25, 0, 0}},
     {"Nylon", {E_STRING, 50, 0, 30, 50, 5, 10, 10, 0, 60, 0, 40, 0, 10}}},
    {{"Mellow Square", {E_SQUARE, 20, 8, 30, 38, 10, 15, 15, 3, 60, 70, 30, 30, 25}},
     {"Analog Brass", {E_BRASS, 45, 12, 20, 45, 10, 35, 30, 6, 60, 65, 25, 15, 15}},
     {"Saw Choir", {E_SAW, 20, 18, 35, 35, 5, 10, 10, 12, 70, 80, 45, 20, 30}},
     {"West Coast Lead", {E_SQUARE, 30, 4, 10, 50, 10, 10, 20, 4, 60, 75, 30, 55, 45}},
     {"Soft Saw", {E_SAW, 25, 10, 20, 40, 5, 15, 10, 5, 60, 70, 35, 25, 20}},
     {"Hollow Pulse", {E_SQUARE, 35, 0, 40, 30, 15, 20, 25, 3, 55, 60, 25, 20, 15}}},
    {{"Music Box", {E_GLASS, 60, 0, 30, 70, 5, 10, 5, 0, 65, 0, 55, 0, 0}},
     {"Glass Mallet", {E_GLASS, 45, 4, 10, 62, 5, 10, 5, 0, 55, 0, 45, 0, 10}},
     {"Toy Celesta", {E_BELL, 35, 0, 45, 60, 5, 10, 10, 0, 50, 0, 40, 0, 0}},
     {"Dream Bell", {E_BELL, 50, 8, 20, 55, 5, 10, 5, 1, 85, 0, 75, 0, 20}},
     {"Tubular", {E_BELL, 65, 0, 15, 50, 5, 10, 10, 0, 90, 0, 80, 0, 0}},
     {"Vinyl Chime", {E_GLASS, 50, 0, 70, 52, 5, 10, 15, 0, 60, 0, 50, 0, 5}}},
    {{"Soul Flute", {E_FLUTE, 40, 0, 20, 60, 5, 10, 5, 8, 60, 85, 30, 15, 40}},
     {"Breathy Pan", {E_FLUTE, 75, 0, 25, 50, 5, 10, 5, 10, 60, 80, 35, 10, 25}},
     {"Ocarina", {E_FLUTE, 15, 0, 10, 45, 5, 5, 5, 6, 60, 90, 25, 15, 30}},
     {"Dusty Flute", {E_FLUTE, 45, 0, 60, 48, 5, 10, 15, 8, 60, 80, 30, 10, 35}},
     {"Low Whistle", {E_FLUTE, 30, 0, 15, 40, 5, 5, 5, 9, 65, 90, 35, 20, 50}},
     {"Air Recorder", {E_FLUTE, 60, 0, 30, 65, 5, 10, 5, 5, 55, 75, 25, 5, 15}}},
};

static const char* const kCaptions[][2] = {
    {"genra", "GENRA"}, {"name", "MELODY"}, {"new", "NEW"}, {"undo", "UNDO"}, {"auto", "AUTO"}, {"padsmode", "PADS"},
    {"keylock", "KEY LOCK"}, {"rhythm", "RHYTHM"}, {"rh1", "CHOP"}, {"rh2", "FREE"}, {"rh3", "SPARSE"},
    {"notes", "NOTES"}, {"no1", "SCALE"}, {"no2", "CHORD"}, {"no3", "PENTA"}, {"pattern", "PATTERN"},
    {"tone", "TONE"}, {"osc", "OSC"}, {"filter", "FILTER"}, {"env", "ENVELOPE"},
    {"cat1", "KEYS"}, {"cat2", "RHODES"}, {"cat3", "PLUCK"}, {"cat4", "LEAD"}, {"cat5", "BELL"}, {"cat6", "FLUTE"},
    {"hint", "Tap a preset to load · double tap = big knob"},
    {"print", "PRINT"}, {"printtitle", "· PRINT"}, {"after", "After PRINT"}, {"autoinfo", "AUTO: the loop plays"},
    {"padsinfo", "PADS: only the MPC track's notes"}, {"r1", "LINK"}, {"r2", "Key"}, {"r3", "Print folder"},
    {"r4", "Version"}};
static const int kNumCaps = (int)(sizeof(kCaptions) / sizeof(kCaptions[0]));

enum
{
    P_TONE = 0, P_BITE, P_DETUNE, P_AGE, P_CUTOFF, P_RESO, P_ENV, P_DRIVE, P_ATTACK, P_DECAY, P_SUSTAIN, P_RELEASE,
    P_GLIDE, P_VIB,
    P_DENSITY, P_SWING, P_RANGE, P_OCT, P_LEVEL,
    P_CAT, P_AUTO, P_KEYLOCK, P_RHYTHM, P_NOTES,
    P_NEW, P_UNDO, P_PRINT, P_PRESET0,
    P_HDRLINK = P_PRESET0 + kPresetRows, P_HDRPRESET, P_STATUS, P_PRINTINFO, P_PRINTSTATUS, P_PRINTDIRTXT,
    P_LINKINFO, P_KEYINFO, P_VERSION,
    P_CHORD0,
    P_PRESETTXT0 = P_CHORD0 + genralink::kBars,
    P_PRINTROW0 = P_PRESETTXT0 + kPresetRows,
    P_CAP0 = P_PRINTROW0 + kPrintRows,
    P_COL0 = P_CAP0 + kNumCaps,
    P_PRESETLIT0 = P_COL0 + kSteps,
    P_FLASH0 = P_PRESETLIT0 + kPresetRows, // NEW, UNDO, PRINT
    P_COUNT = P_FLASH0 + 3
};

struct Note { int8_t pitch = -1; float len = 1.f; float vel = 0.8f; float off = 0.f; bool slide = false, acc = false; };
struct Line { Note n[kSteps]; float swing = 0.5f; bool valid = false; };

// a fast sine: 4096-point table, linear interpolation, phase in cycles
struct SineTable
{
    float t[4097];
    SineTable() { for (int i = 0; i <= 4096; ++i) t[i] = (float)std::sin(i * 2.0 * M_PI / 4096.0); }
};
static const SineTable kSine;
inline float sn(float ph)
{
    ph -= std::floor(ph);
    float x = ph * 4096.f;
    int i = (int)x;
    float f = x - (float)i;
    return kSine.t[i] + f * (kSine.t[i + 1] - kSine.t[i]);
}

} // namespace

class GenraMelody : public Plugin
{
public:
    GenraMelody() : Plugin(makeInfo()), screen_(*this)
    {
        std::vector<std::string> engines(kEngineNames, kEngineNames + NUM_ENGINES);
        addParam(choiceParam("tone", "TONE", engines, E_EPIANO));
        addParam(floatParam("bite", "BITE", 0.f, 100.f, 40.f, "%", 0));
        addParam(floatParam("detune", "DETUNE", 0.f, 50.f, 6.f, "ct", 0));
        addParam(floatParam("age", "AGE", 0.f, 100.f, 30.f, "%", 0));
        addParam(floatParam("cutoff", "CUTOFF", 0.f, 100.f, 55.f, "", 0));
        addParam(floatParam("reso", "RESO", 0.f, 100.f, 5.f, "%", 0));
        addParam(floatParam("env", "ENV", 0.f, 100.f, 15.f, "%", 0));
        addParam(floatParam("drive", "DRIVE", 0.f, 100.f, 15.f, "%", 0));
        addParam(floatParam("attack", "ATTACK", 0.f, 100.f, 1.f, "", 0));
        addParam(floatParam("decay", "DECAY", 0.f, 100.f, 60.f, "", 0));
        addParam(floatParam("sustain", "SUSTAIN", 0.f, 100.f, 40.f, "%", 0));
        addParam(floatParam("release", "RELEASE", 0.f, 100.f, 35.f, "", 0));
        addParam(floatParam("glide", "GLIDE", 0.f, 100.f, 10.f, "", 0));
        addParam(floatParam("vibrato", "VIBRATO", 0.f, 100.f, 20.f, "%", 0));
        addParam(floatParam("density", "DENSITY", 0.f, 100.f, 100.f, "%", 0));
        addParam(floatParam("swing", "SWING", -25.f, 25.f, 0.f, "%", 0));
        addParam(intParam("range", "RANGE", 1, 3, 2));
        addParam(intParam("oct", "OCTAVE", -2, 2, 0));
        addParam(floatParam("level", "LEVEL", -40.f, 6.f, -6.f, "dB", 1));
        addParam(choiceParam("cat", "KIND", {kCatNames[0], kCatNames[1], kCatNames[2], kCatNames[3], kCatNames[4], kCatNames[5]}, 1));
        addParam(choiceParam("auto", "AUTO PADS", {"Auto", "Pads"}, 0));
        addParam(boolParam("keylock", "KEY LOCK", true));
        addParam(choiceParam("rhythm", "RHYTHM", {"Chop", "Free", "Sparse"}, 1));
        addParam(choiceParam("notes", "NOTES", {"Scale", "Chord", "Penta"}, 0));
        addButton("NEW");
        addButton("UNDO");
        addButton("PRINT");
        for (int i = 0; i < kPresetRows; ++i) addButton(("PRESET " + std::to_string(i + 1)).c_str());
        for (const char* t : {"KOPFLINK", "KOPFPRESET", "STATUS", "PRINTINFO", "PRINTSTATUS", "PRINTORDNER", "LINKINFO",
                              "TONARTINFO", "VERSION"})
            addText(t);
        for (int i = 0; i < genralink::kBars; ++i) addText(("AKKORD " + std::to_string(i + 1)).c_str());
        for (int i = 0; i < kPresetRows; ++i) addText(("PRESETNAME " + std::to_string(i + 1)).c_str());
        for (int i = 0; i < kPrintRows; ++i) addText(("PRINTZEILE " + std::to_string(i + 1)).c_str());
        for (const auto& c : kCaptions) addText((std::string("CAP ") + c[0]).c_str());
        for (int i = 0; i < kSteps; ++i) addText(("LINIE " + std::to_string(i + 1)).c_str());
        for (int i = 0; i < kPresetRows; ++i) addText(("PRESETLICHT " + std::to_string(i + 1)).c_str());
        for (int i = 0; i < 3; ++i) addText(("BLITZ " + std::to_string(i + 1)).c_str());
        if (numParams() != P_COUNT) std::abort();

        screen_.init();
        for (int i = P_HDRLINK; i < P_COL0; ++i) screen_.text(i);
        for (int i = 0; i < kNumCaps; ++i) screen_.text(P_CAP0 + i, kCaptions[i][1]);
        for (int i = 0; i < kSteps; ++i) screen_.strip(P_COL0 + i, kColFrames);
        for (int i = 0; i < kPresetRows; ++i) { screen_.strip(P_PRESETLIT0 + i, 2); screen_.button(P_PRESET0 + i); }
        screen_.button(P_NEW, P_FLASH0);
        screen_.button(P_UNDO, P_FLASH0 + 1);
        screen_.button(P_PRINT, P_FLASH0 + 2);
        screen_.watch(P_CAT);
        screen_.watch(P_AUTO);
        screen_.setText(P_VERSION, "GENRA MELODY 0.1 · LINK v3");
    }

    ~GenraMelody() override
    {
        quit_.store(true);
        if (worker_.joinable()) worker_.join();
    }

    static PluginInfo makeInfo()
    {
        PluginInfo i;
        i.name = "GENRA MELODY";
        i.vendor = "Genra";
        i.uniqueId = MPC_FOURCC('G', 'n', 'M', 'l');
        i.version = 100;
        i.isInstrument = true;
        i.numInputs = 0;
        i.numOutputs = 2;
        return i;
    }

    void prepare(double sampleRate, int) override
    {
        sr_ = sampleRate;
        oscA_.setSampleRate(sampleRate);
        oscB_.setSampleRate(sampleRate);
        filt_.setSampleRate(sampleRate);
        breath_.setSampleRate(sampleRate);
        if (!started_)
        {
            started_ = true;
            link_.open();
            worker_ = std::thread([this] { workerMain(); });
        }
    }

    void reset() override
    {
        env_ = 0.f; fenv_ = 0.f; menv_ = 0.f; tenv_ = 0.f; gainSmooth_ = -1.f; filt_.reset(); breath_.reset();
        gate_ = -1.0; wasPlaying_ = false; nPending_ = 0; stage_ = 0; nHeld_ = 0;
        for (float& k : ks_) k = 0.f;
    }

    void process(ProcessContext& ctx) override
    {
        genralink::LinkData* L = link_.data();
        if (L) L->lane[genralink::LANE_MEL].beat.fetch_add(1, std::memory_order_relaxed);
        const bool keyLock = getBool(P_KEYLOCK);
        const bool autoMode = getInt(P_AUTO) == 0;
        const int oct = getInt(P_OCT);
        engine_ = clamp(getInt(P_TONE), 0, NUM_ENGINES - 1);

        int r = ready_.load(std::memory_order_acquire);
        if (r >= 0) inUse_.store(r, std::memory_order_release);
        const Line* line = r >= 0 ? &lines_[r] : nullptr;

        const Transport& tr = ctx.transport;
        const bool playing = tr.valid && tr.playing && tr.ppqValid;
        const double bpm = tr.tempoValid && tr.bpm > 1.0 ? tr.bpm : 120.0;
        const double spStep = sr_ * 60.0 / bpm / 4.0;
        if (playing && autoMode && line && line->valid)
        {
            const double q0 = tr.ppqPos * 4.0, q1 = q0 + ctx.numFrames / spStep;
            if (!wasPlaying_ || nextStep_ < q0 - 1.0 || nextStep_ > q1 + 1.0) nextStep_ = std::ceil(q0 - 1e-6);
            const float swing = clampf(line->swing + get(P_SWING) / 100.f, 0.5f, 0.75f);
            while (nextStep_ < q1)
            {
                long n = (long)std::llround(nextStep_);
                int s = (int)(((n % kSteps) + kSteps) % kSteps);
                const Note& sn0 = line->n[s];
                if (sn0.pitch >= 0 && nPending_ < 8)
                {
                    double at = (nextStep_ - q0) * spStep;
                    if (s & 1) at += (swing - 0.5f) * 2.f * spStep;
                    if (sn0.off > 0.f) at += sn0.off * spStep;
                    Pending& p = pending_[nPending_++];
                    p.at = at;
                    p.note = clampNote(sn0.pitch + 12 * oct);
                    p.vel = sn0.vel * (sn0.acc ? 1.f : 0.85f);
                    p.acc = sn0.acc;
                    p.slide = sn0.slide;
                    p.gate = std::max(0.2, (double)sn0.len) * spStep;
                }
                curStep_.store(s, std::memory_order_relaxed);
                nextStep_ += 1.0;
            }
            wasPlaying_ = true;
        }
        else
        {
            if (wasPlaying_ && gate_ > 0.0) gate_ = 0.0;
            wasPlaying_ = false;
            nPending_ = 0;
            curStep_.store(playing ? curStepOf(tr) : -1, std::memory_order_relaxed);
        }

        // level, with the filter's level compensation: the more the low-pass is closed, the more it is lifted
        const float cutoff = get(P_CUTOFF) / 100.f;
        const float comp = std::pow(10.f, 9.f * std::pow(1.f - cutoff, 1.5f) / 20.f);
        float target = std::pow(10.f, get(P_LEVEL) / 20.f) * comp;
        if (gainSmooth_ < 0.f) gainSmooth_ = target;
        const float envAmt = get(P_ENV) / 100.f;
        // no "ui": the more filter envelope, the less resonance; never near self-oscillation
        const float reso = 0.02f + 0.6f * (get(P_RESO) / 100.f) * (1.f - 0.65f * envAmt);
        const float drive = 1.f + 4.f * get(P_DRIVE) / 100.f;
        const float bite = get(P_BITE) / 100.f, detune = get(P_DETUNE), age = get(P_AGE) / 100.f;
        const float vib = get(P_VIB) / 100.f;
        const float att = 0.001f + 0.8f * std::pow(get(P_ATTACK) / 100.f, 2.f);
        const float dec = 0.05f + 3.f * std::pow(get(P_DECAY) / 100.f, 2.f);
        const float sus = get(P_SUSTAIN) / 100.f;
        const float rel = 0.02f + 2.f * std::pow(get(P_RELEASE) / 100.f, 2.f);
        const float attC = 1.f / (float)(sr_ * att);
        const float decC = std::exp(-1.f / (float)(sr_ * dec));
        const float relC = std::exp(-1.f / (float)(sr_ * rel));
        const float fdec = std::exp(-1.f / (float)(sr_ * (0.04 + 0.5 * dec)));
        const float mdec = std::exp(-1.f / (float)(sr_ * (0.12 + 0.6 * dec)));       // FM index / partials
        const float tdec = std::exp(-1.f / (float)(sr_ * 0.03));                      // tine, click
        const float glideT = 0.005f + 0.25f * get(P_GLIDE) / 100.f;
        const float glideC = 1.f - std::exp(-1.f / (float)(sr_ * glideT));
        const float ksDamp = 0.25f + 0.7f * bite;
        const float ksFeedback = 0.990f + 0.0095f * (get(P_DECAY) / 100.f);
        const float driveNorm = std::tanh(drive);
        const float cents = std::pow(2.f, detune / 1200.f);
        const float dullC = 1.f - std::exp(-2.f * (float)M_PI * (16000.f - 11500.f * age) / (float)sr_);
        const float isr = 1.f / (float)sr_;
        const float crush = age > 0.6f ? std::pow(2.f, 15.f - (age - 0.6f) * 22.f) : 0.f; // AGE above 60 %: fewer bits
        float* outL = ctx.outputs[0];
        float* outR = ctx.numOutputs > 1 ? ctx.outputs[1] : nullptr;
        int ev = 0;
        for (int i = 0; i < ctx.numFrames; ++i)
        {
            while (ev < ctx.numMidiEvents && ctx.midi[ev].sampleOffset <= i)
            {
                const MidiEvent& e = ctx.midi[ev++];
                if (e.isNoteOn())
                {
                    int note = clampNote((keyLock ? snapToKey(e.note(), L) : e.note()) + 12 * oct);
                    bool legato = nHeld_ > 0;
                    pushHeld(e.note(), note);
                    noteOn(note, e.velocity() / 127.f, false, legato, -1.0);
                }
                else if (e.isNoteOff())
                {
                    int back = popHeld(e.note());
                    if (back >= 0) noteOn(back, vel_, false, true, -1.0); // glide back to the key still held
                    else if (nHeld_ == 0) gate_ = 0.0;
                }
                else if (e.isAllNotesOff()) { nHeld_ = 0; gate_ = 0.0; }
            }
            for (int k = 0; k < nPending_;)
            {
                if (pending_[k].at <= (double)i)
                {
                    const Pending& p = pending_[k];
                    noteOn(p.note, p.vel, p.acc, p.slide, p.gate);
                    pending_[k] = pending_[--nPending_];
                }
                else ++k;
            }
            gainSmooth_ += (target - gainSmooth_) * 0.002f;
            if (gate_ > 0.0) { gate_ -= 1.0; if (gate_ < 0.0) gate_ = 0.0; }
            const bool held = gate_ < 0.0 || gate_ > 0.0;
            if (!held && stage_ != 0) stage_ = 0;
            if (stage_ == 1) { env_ += attC; if (env_ >= 1.f) { env_ = 1.f; stage_ = 2; } }
            else if (stage_ == 2) env_ = sus + (env_ - sus) * decC;
            else env_ *= relC;
            fenv_ *= fdec;
            menv_ *= mdec;
            tenv_ *= tdec;
            hz_ += (targetHz_ - hz_) * glideC;
            noteT_ += isr;

            // pitch: vibrato that comes in late, tape wow and flutter
            lfoV_ += 5.2f * isr; if (lfoV_ >= 1.f) lfoV_ -= 1.f;
            lfoW_ += 0.55f * isr; if (lfoW_ >= 1.f) lfoW_ -= 1.f;
            lfoF_ += 7.3f * isr; if (lfoF_ >= 1.f) lfoF_ -= 1.f;
            const bool tremolo = engine_ <= E_VIBES;
            const float vibIn = std::min(1.f, std::max(0.f, (noteT_ - 0.18f) * 3.f));
            float cent = age * (10.f * sn(lfoW_) + 2.5f * sn(lfoF_));
            if (!tremolo) cent += vib * vibIn * 22.f * sn(lfoV_);
            const float f = hz_ * std::pow(2.f, cent / 1200.f);

            if ((ctrl_++ & 15) == 0)
            {
                oscA_.setWave(engine_ == E_SQUARE ? mpc::dsp::Wave::Square : mpc::dsp::Wave::Saw);
                oscB_.setWave(engine_ == E_SQUARE ? mpc::dsp::Wave::Square : mpc::dsp::Wave::Saw);
                oscA_.setFrequency(f);
                oscB_.setFrequency(f * cents);
                float track = std::sqrt(std::max(30.f, f) / 261.6f);
                float cut = 60.f * std::pow(2.f, cutoff * 8.2f) * track * (1.f + 3.f * envAmt * fenv_ * vel_);
                filt_.set(std::fmin(cut, 15000.f), reso);
                breath_.set(std::fmin(f * 2.f, 9000.f), 0.6f);
            }
            const float inc = f * isr;
            float raw = 0.f;
            switch (engine_)
            {
                case E_EPIANO:
                {
                    ph1_ += inc; ph2_ += inc; ph3_ += inc * 14.f; ph4_ += inc * cents;
                    float idx = (0.25f + 2.2f * bite * vel_) * menv_;
                    float mod = sn(ph2_);
                    raw = 0.65f * sn(ph1_ + idx * mod * 0.16f) + 0.35f * sn(ph4_ + idx * mod * 0.12f);
                    raw += 0.22f * bite * tenv_ * sn(ph3_);
                    break;
                }
                case E_WURLI:
                {
                    ph1_ += inc; ph2_ += inc; ph4_ += inc * cents;
                    float idx = (0.6f + 2.4f * bite * vel_) * (0.35f + 0.65f * menv_);
                    float c = 0.6f * sn(ph1_ + idx * sn(ph2_) * 0.15f) + 0.4f * sn(ph4_);
                    raw = (std::tanh(1.8f * c + 0.35f) - 0.3364f) * 0.9f;
                    break;
                }
                case E_CLAV:
                    raw = pluck(0.75f + 0.24f * bite, ksFeedback - 0.004f) * 2.2f;
                    raw = std::tanh(raw * (1.f + 1.5f * bite));
                    break;
                case E_VIBES:
                    ph1_ += inc; ph3_ += inc * 4.f;
                    raw = 0.75f * sn(ph1_) + 0.3f * bite * menv_ * sn(ph3_);
                    break;
                case E_STRING:
                    raw = pluck(ksDamp, ksFeedback) * 2.6f;
                    break;
                case E_KALIMBA:
                    ph1_ += inc; ph3_ += inc * 5.93f;
                    raw = 0.8f * sn(ph1_) + 0.45f * bite * menv_ * menv_ * sn(ph3_);
                    break;
                case E_SQUARE:
                    raw = 0.55f * oscA_.process() + (detune > 0.5f ? 0.35f * oscB_.process() : 0.f);
                    break;
                case E_SAW:
                case E_BRASS:
                    raw = 0.5f * oscA_.process() + 0.5f * oscB_.process();
                    if (engine_ == E_BRASS) raw = std::tanh(raw * (1.2f + bite));
                    break;
                case E_BELL:
                {
                    ph1_ += inc; ph2_ += inc * 3.5f; ph4_ += inc * cents;
                    float idx = (0.3f + 2.6f * bite * vel_) * menv_;
                    raw = 0.7f * sn(ph1_ + idx * sn(ph2_) * 0.16f) + 0.3f * sn(ph4_);
                    break;
                }
                case E_GLASS:
                    ph1_ += inc; ph3_ += inc * 2.76f; ph4_ += inc * 5.4f;
                    raw = 0.7f * sn(ph1_) + 0.3f * bite * menv_ * sn(ph3_) + 0.12f * bite * tenv_ * sn(ph4_);
                    break;
                case E_FLUTE:
                {
                    ph1_ += inc;
                    float b = breath_.process(noise(), mpc::dsp::SVF::Mode::Bandpass);
                    float breathAmt = bite * (0.10f + 0.35f * tenv_);
                    raw = 0.8f * sn(ph1_) + 0.1f * sn(2.f * ph1_) + breathAmt * b * 2.f;
                    break;
                }
                default: break;
            }
            if (ph1_ > 64.f) { ph1_ -= std::floor(ph1_); ph2_ -= std::floor(ph2_); ph3_ -= std::floor(ph3_); ph4_ -= std::floor(ph4_); }
            if (click_ > 0) { raw += 0.15f * bite * noise() * ((float)click_ / (float)clickLen_); --click_; }
            float y = filt_.process(raw, mpc::dsp::SVF::Mode::Lowpass);
            y = std::tanh(y * drive) / driveNorm;
            float amp = env_ * vel_;
            if (tremolo) amp *= 1.f - 0.45f * vib * (0.5f + 0.5f * sn(lfoV_ * 0.9f));
            y *= amp * gainSmooth_ * 1.0f;
            y += age * 0.004f * noise() * gainSmooth_;
            dull_ += dullC * (y - dull_);
            y = dull_;
            if (crush > 0.f) y = std::round(y * crush) / crush;
            y = std::tanh(y * 1.1f) / 1.1f; // soft limiter
            outL[i] = y;
            if (outR) outR[i] = y;
        }
        for (int k = 0; k < nPending_; ++k) pending_[k].at -= ctx.numFrames;
        inUse_.store(-1, std::memory_order_release);
    }

    void saveExtraState(std::vector<uint8_t>& out) override
    {
        std::string s = "GM1\n" + std::to_string(presetRow_.load()) + "\n";
        out.assign(s.begin(), s.end());
    }
    void loadExtraState(const uint8_t* data, size_t size) override
    {
        std::string s(reinterpret_cast<const char*>(data), size);
        if (s.compare(0, 4, "GM1\n") == 0) presetRow_.store(std::atoi(s.c_str() + 4));
        resyncReq_.store(true);
    }

    bool formatValue(int index, float plain, std::string& out) override
    {
        char b[32];
        switch (index)
        {
            case P_CUTOFF:
            {
                float hz = 60.f * std::pow(2.f, plain / 100.f * 8.2f);
                if (hz >= 1000.f) std::snprintf(b, sizeof b, "%.1fk", hz / 1000.f);
                else std::snprintf(b, sizeof b, "%.0f", hz);
                out = b;
                return true;
            }
            case P_ATTACK: std::snprintf(b, sizeof b, "%.0f ms", (0.001f + 0.8f * std::pow(plain / 100.f, 2.f)) * 1000.f); out = b; return true;
            case P_DECAY: std::snprintf(b, sizeof b, "%.0f ms", (0.05f + 3.f * std::pow(plain / 100.f, 2.f)) * 1000.f); out = b; return true;
            case P_RELEASE: std::snprintf(b, sizeof b, "%.0f ms", (0.02f + 2.f * std::pow(plain / 100.f, 2.f)) * 1000.f); out = b; return true;
            case P_GLIDE: std::snprintf(b, sizeof b, "%.0f ms", (0.005f + 0.25f * plain / 100.f) * 1000.f); out = b; return true;
            case P_SWING:
                if (std::fabs(plain) < 0.5f) out = "from chop";
                else { std::snprintf(b, sizeof b, "%+.0f %%", plain); out = b; }
                return true;
            case P_RANGE: std::snprintf(b, sizeof b, "%d oct", (int)std::lround(plain)); out = b; return true;
            case P_OCT: std::snprintf(b, sizeof b, "%+d", (int)std::lround(plain)); out = b; return true;
            case P_DETUNE: std::snprintf(b, sizeof b, "%.0f ct", plain); out = b; return true;
            case P_LEVEL: std::snprintf(b, sizeof b, "%+.1f dB", plain); out = b; return true;
            case P_BITE: case P_AGE: case P_RESO: case P_ENV: case P_DRIVE: case P_SUSTAIN: case P_DENSITY: case P_VIB:
                std::snprintf(b, sizeof b, "%.0f %%", plain); out = b; return true;
            default: break;
        }
        if (index >= P_COL0) return false;
        return screen_.format(index, out);
    }

private:
    struct Pending { double at; int note; float vel; bool acc, slide; double gate; };

    static float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
    static int clampNote(int n) { return n < 24 ? 24 : (n > 108 ? 108 : n); }
    static int curStepOf(const Transport& tr) { long n = (long)std::floor(tr.ppqPos * 4.0); return (int)(((n % kSteps) + kSteps) % kSteps); }

    void addText(const char* name)
    {
        std::string id = name;
        for (char& c : id) c = c == ' ' ? '_' : (char)::tolower((unsigned char)c);
        Param p = floatParam(id.c_str(), name, 0.f, 1.f, 0.f);
        p.automatable = false;
        addParam(p);
    }
    void addButton(const char* name)
    {
        std::string id = name;
        for (char& c : id) c = c == ' ' ? '_' : (char)::tolower((unsigned char)c);
        addParam(boolParam(id.c_str(), name, false));
    }

    float noise()
    {
        rng_ ^= rng_ << 13; rng_ ^= rng_ >> 17; rng_ ^= rng_ << 5;
        return (rng_ >> 8) * (1.f / 8388608.f) - 1.f;
    }

    // a plucked string: a delay line one period long, filled with noise on the hit, damped every round
    float pluck(float damp, float feedback)
    {
        if (ksLen_ < 2) return 0.f;
        int i = ksPos_;
        int j = i + 1 >= ksLen_ ? 0 : i + 1;
        float y = ks_[i];
        float avg = 0.5f * (ks_[i] + ks_[j]);
        ksLp_ += damp * (avg - ksLp_);
        ks_[i] = ksLp_ * feedback;
        ksPos_ = j;
        return y * 1.6f;
    }

    // keys held on the MPC (pads mode): the last one sounds, letting go glides back
    void pushHeld(int key, int note)
    {
        popHeld(key);
        if (nHeld_ < 8) { heldKey_[nHeld_] = key; heldNote_[nHeld_] = note; ++nHeld_; }
    }
    int popHeld(int key)
    {
        bool wasTop = nHeld_ > 0 && heldKey_[nHeld_ - 1] == key;
        for (int k = 0; k < nHeld_; ++k)
            if (heldKey_[k] == key)
            {
                for (int j = k; j + 1 < nHeld_; ++j) { heldKey_[j] = heldKey_[j + 1]; heldNote_[j] = heldNote_[j + 1]; }
                --nHeld_;
                break;
            }
        return wasTop && nHeld_ > 0 ? heldNote_[nHeld_ - 1] : -1;
    }

    // gate: samples the note is held (loop), -1: until note-off (keys)
    void noteOn(int note, float vel, bool acc, bool slide, double gate)
    {
        float hz = (float)(440.0 * std::pow(2.0, (note - 69) / 12.0));
        targetHz_ = hz;
        const bool legato = slide && env_ > 0.05f;
        if (!legato)
        {
            hz_ = hz;
            fenv_ = 1.f;
            menv_ = acc ? 1.3f : 1.f;
            tenv_ = 1.f;
            stage_ = 1;
            noteT_ = 0.f;
            vel_ = 0.35f + 0.65f * vel;
            click_ = clickLen_ = std::max(1, (int)(sr_ * 0.004));
            if (engine_ == E_STRING || engine_ == E_CLAV)
            {
                ksLen_ = std::max(2, std::min((int)(sizeof(ks_) / sizeof(ks_[0])), (int)std::lround(sr_ / hz)));
                float lp = 0.f;
                const float bright = engine_ == E_CLAV ? 0.9f : 0.5f;
                for (int k = 0; k < ksLen_; ++k) { lp += bright * (noise() - lp); ks_[k] = lp; }
                ksPos_ = 0;
                ksLp_ = 0.f;
            }
        }
        gate_ = gate;
    }

    static int snapToKey(int note, genralink::LinkData* L)
    {
        if (!L) return note;
        int root = L->keyRoot.load(std::memory_order_relaxed);
        if (root < 0 || root > 11) return note;
        const int* sc = L->keyMinor.load(std::memory_order_relaxed) ? kMinor : kMajor;
        int rel = ((note - root) % 12 + 12) % 12;
        int best = 0, bestD = 99;
        for (int k = 0; k < 7; ++k) { int d = std::abs(sc[k] - rel); if (d < bestD) { bestD = d; best = sc[k]; } }
        return note - rel + best;
    }
    static int snapTo(int note, int root, const int* sc, int n)
    {
        if (root < 0 || root > 11) return note;
        int rel = ((note - root) % 12 + 12) % 12;
        int best = 0, bestD = 99;
        for (int k = 0; k < n; ++k) { int d = std::abs(sc[k] - rel); if (d < bestD) { bestD = d; best = sc[k]; } }
        return note - rel + best;
    }

    // ------------------------------------------------------------------ worker: the line from the loop
    static int nearestChordTone(int pitch, int root, int q)
    {
        if (root < 0 || q < 0 || q > 7) return pitch;
        int best = pitch, bestD = 99;
        for (int t = 0; t < 5; ++t)
        {
            int tone = kQualTones[q][t];
            if (tone < 0) continue;
            int pc = (root + tone) % 12;
            for (int o = -1; o <= 1; ++o)
            {
                int cand = (pitch / 12 + o) * 12 + pc;
                int d = std::abs(cand - pitch);
                if (d < bestD) { bestD = d; best = cand; }
            }
        }
        return best;
    }

    void buildLine(const genralink::Loop& l, int keyRoot, bool keyMinor, Line& out)
    {
        const int rhythm = getInt(P_RHYTHM), notes = getInt(P_NOTES), range = clamp(getInt(P_RANGE), 1, 3);
        const float dens = get(P_DENSITY) / 100.f;
        const int densLimit = dens >= 0.999f ? 256 : (int)std::lround(dens * 254.f);
        out = Line();
        out.swing = l.swingX1000 / 1000.f;
        out.valid = true;
        int lastPitch = -1;
        int pitchAt[kSteps];
        for (int s = 0; s < kSteps; ++s) { if (l.mel[s].pitch >= 0) lastPitch = l.mel[s].pitch; pitchAt[s] = lastPitch; }
        if (lastPitch >= 0) for (int s = 0; s < kSteps && pitchAt[s] < 0; ++s) pitchAt[s] = lastPitch;
        for (int s = 0; s < kSteps; ++s)
        {
            Note n;
            const genralink::StepNote& m = l.mel[s];
            if (rhythm == 1 || rhythm == 2)
            {
                const int limit = rhythm == 2 ? std::min(densLimit, 90) : densLimit;
                if (m.pitch < 0 || (int)m.prio > limit) continue;
                n.pitch = m.pitch; n.len = m.len16 / 16.f; n.vel = m.vel / 127.f; n.off = m.off / 100.f;
                n.slide = (m.flags & 2) != 0; n.acc = (m.flags & 1) != 0;
                if (rhythm == 2) // sparse: the main notes ring on to the next one
                {
                    int next = s + 1;
                    while (next < kSteps && !(l.mel[next].pitch >= 0 && (int)l.mel[next].prio <= limit)) ++next;
                    n.len = (float)std::min(8, next - s) - 0.1f;
                }
            }
            else
            {
                if (l.chop[s] == 0 || pitchAt[s] < 0) continue;
                uint32_t h = (uint32_t)s * 2654435761u ^ l.id * 40503u;
                if (dens < 0.999f && ((h >> 8) & 255) / 255.f > dens) continue;
                int next = s + 1;
                while (next < kSteps && l.chop[next] == 0) ++next;
                n.pitch = (int8_t)pitchAt[s];
                n.len = (float)std::min(4, next - s) - 0.1f;
                n.vel = 0.8f;
            }
            const int bar = s / 16;
            const int root = l.barRoot[bar], q = l.barQuality[bar];
            if (notes == 1) n.pitch = (int8_t)nearestChordTone(n.pitch, root, q);
            else if (notes == 2) n.pitch = (int8_t)snapTo(n.pitch, keyRoot, keyMinor ? kPentaMinor : kPentaMajor, 5);
            else n.pitch = (int8_t)snapTo(n.pitch, keyRoot, keyMinor ? kMinor : kMajor, 7);
            out.n[s] = n;
        }
        // RANGE: fold the line into 1..3 octaves around its middle
        int lo = 127, hi = 0;
        for (int s = 0; s < kSteps; ++s) if (out.n[s].pitch >= 0) { lo = std::min(lo, (int)out.n[s].pitch); hi = std::max(hi, (int)out.n[s].pitch); }
        if (hi >= lo)
        {
            const int span = 12 * range, mid = (lo + hi) / 2, bottom = mid - span / 2;
            for (int s = 0; s < kSteps; ++s)
            {
                int p = out.n[s].pitch;
                if (p < 0) continue;
                while (p < bottom) p += 12;
                while (p > bottom + span) p -= 12;
                out.n[s].pitch = (int8_t)p;
            }
        }
    }

    void publishLine(const Line& line)
    {
        int r = ready_.load(), u = inUse_.load(), w = 0;
        while (w == r || w == u) ++w;
        lines_[w] = line;
        ready_.store(w, std::memory_order_release);
        shown_ = line;
    }

    // ------------------------------------------------------------------ worker: screen
    void lineColumns()
    {
        int lo = 127, hi = 0;
        for (int s = 0; s < kSteps; ++s) if (shown_.n[s].pitch >= 0) { lo = std::min(lo, (int)shown_.n[s].pitch); hi = std::max(hi, (int)shown_.n[s].pitch); }
        const int step = curStep_.load();
        int held[kSteps] = {};
        for (int s = 0; s < kSteps; ++s)
        {
            const Note& n = shown_.n[s];
            if (n.pitch < 0) continue;
            int h = hi > lo ? 1 + (int)std::lround((n.pitch - lo) * 15.0 / std::max(1, hi - lo)) : 8;
            int len = std::max(1, (int)std::lround(n.len));
            for (int k = 0; k < len && s + k < kSteps; ++k) held[s + k] = h;
        }
        for (int s = 0; s < kSteps; ++s) screen_.setFrame(P_COL0 + s, held[s] + (s == step ? 17 : 0));
    }

    long long nowMs() const
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    static std::string chordName(int r, int q)
    {
        if (r < 0 || r > 11) return "-";
        return std::string(kNotes[r]) + (q >= 0 && q < 8 ? kQual[q] : "");
    }

    std::string keyOf(genralink::LinkData* L) const
    {
        int kr = L ? L->keyRoot.load() : -1;
        return kr >= 0 && kr < 12 ? std::string(kNotes[kr]) + (L->keyMinor.load() ? "m" : "") : std::string("-");
    }

    void texts(genralink::LinkData* L, bool alive)
    {
        std::string key = keyOf(L);
        int bpm100 = L ? L->bpmX100.load() : 0;
        char b[64];
        if (alive) std::snprintf(b, sizeof b, "LINK GENRA CHOP · %s · %.0f BPM", key.c_str(), bpm100 / 100.0);
        else std::snprintf(b, sizeof b, "no GENRA CHOP");
        screen_.setText(P_HDRLINK, b);
        int cat = clamp(getInt(P_CAT), 0, kCats - 1), row = presetRow_.load();
        bool inCat = row >= 0 && row < kPresetRows && presetCat_ == cat;
        std::string pname = row >= 0 && row < kPresetRows ? kPresets[presetCat_][row].name : std::string("own sound");
        if (edited_) pname += " *";
        screen_.setText(P_HDRPRESET, std::string(kCatNames[presetCat_]) + " · " + pname);
        for (int r = 0; r < kPresetRows; ++r)
        {
            bool sel = inCat && r == row;
            screen_.setText(P_PRESETTXT0 + r, std::string(sel ? "> " : "") + kPresets[cat][r].name);
            screen_.setFrame(P_PRESETLIT0 + r, sel ? 1 : 0);
        }
        for (int bar = 0; bar < genralink::kBars; ++bar) screen_.setText(P_CHORD0 + bar, chordName(loop_.barRoot[bar], loop_.barQuality[bar]));
        const char* rh[3] = {"melody on the chops", "the engine's rhythm", "only the main notes"};
        const int rhythm = clamp(getInt(P_RHYTHM), 0, 2);
        int step = curStep_.load();
        if (step >= 0)
        {
            int bar = step / 16;
            std::snprintf(b, sizeof b, "Bar %d · %s · %s", bar + 1, chordName(loop_.barRoot[bar], loop_.barQuality[bar]).c_str(), rh[rhythm]);
            screen_.setText(P_STATUS, b);
        }
        else screen_.setText(P_STATUS, std::string(getInt(P_AUTO) == 0 ? "AUTO · " : "PADS · ") + rh[rhythm]);
        std::snprintf(b, sizeof b, "Melody · 4 bars · %.0f BPM · %s", bpm100 / 100.0, key.c_str());
        screen_.setText(P_PRINTINFO, b);
        screen_.setText(P_PRINTSTATUS, getInt(P_AUTO) == 0 ? "AUTO: the loop plays" : "PADS: only the MPC track's notes");
        screen_.setText(P_LINKINFO, !L ? "Error: " + link_.error() : (alive ? std::string("GENRA CHOP linked") : std::string("no GENRA CHOP")));
        screen_.setText(P_KEYINFO, key);
    }

    void printList()
    {
        std::string dir = printDir(std::string());
        screen_.setText(P_PRINTDIRTXT, dir.empty() ? std::string("no folder") : pathLabel(dir), true);
        std::vector<std::string> list = dir.empty() ? std::vector<std::string>() : listPrints(dir);
        for (int r = 0; r < kPrintRows; ++r)
            screen_.setText(P_PRINTROW0 + r, r < (int)list.size() ? (r == 0 ? "> " : "") + list[(size_t)r].substr(0, list[(size_t)r].size() - 4) : std::string());
    }

    void applyPreset(int cat, int row)
    {
        const Preset& p = kPresets[cat][row];
        for (int k = 0; k < kSoundN; ++k) { set(P_TONE + k, p.v[k]); notifyHostParameterChanged(P_TONE + k); }
        presetRow_.store(row);
        presetCat_ = cat;
        rememberSound();
        edited_ = false;
        if (genralink::LinkData* L = link_.data()) genralink::Link::writeName(L->lane[genralink::LANE_MEL], p.name);
    }
    void rememberSound() { for (int k = 0; k < kSoundN; ++k) soundSeen_[k] = get(P_TONE + k); }
    bool soundChanged() const
    {
        for (int k = 0; k < kSoundN; ++k) if (std::fabs(get(P_TONE + k) - soundSeen_[k]) > 1e-3f) return true;
        return false;
    }

    void print(genralink::LinkData* L)
    {
        std::string dir = printDir(std::string());
        if (dir.empty()) { screen_.message(P_PRINTSTATUS, "no folder to write to"); return; }
        const int oct = getInt(P_OCT);
        const float swing = clampf(shown_.swing + get(P_SWING) / 100.f, 0.5f, 0.75f);
        std::vector<MidiNote> notes;
        for (int s = 0; s < kSteps; ++s)
        {
            const Note& n = shown_.n[s];
            if (n.pitch < 0) continue;
            double at = s + ((s & 1) ? (swing - 0.5f) * 2.f : 0.f) + std::max(0.f, n.off);
            double len = std::max(0.2, (double)n.len);
            // a slide into the next note: let this one overlap it (legato), so the MPC glides too
            for (int k = s + 1; k < kSteps && k <= s + 8; ++k)
                if (shown_.n[k].pitch >= 0) { if (shown_.n[k].slide) len = std::max(len, (double)(k - s) + 0.25); break; }
            int vel = (int)std::lround(std::min(1.f, n.vel * (n.acc ? 1.f : 0.85f)) * 127.f);
            notes.push_back({at, len, clampNote(n.pitch + 12 * oct), std::max(1, vel)});
        }
        int bpm100 = L ? L->bpmX100.load() : 0;
        double bpm = bpm100 > 0 ? bpm100 / 100.0 : 120.0;
        int no = printNumberFor(dir, loop_.id);
        char name[96];
        std::snprintf(name, sizeof name, "Print %02d Melody %s %.0f", no, keyOf(L).c_str(), bpm);
        std::vector<uint8_t> f = midiFile(notes, bpm, name, kSteps);
        if (writeFileBytes(dir + "/" + name + ".mid", f.data(), f.size()))
        {
            set(P_AUTO, 1.f);
            notifyHostParameterChanged(P_AUTO);
            screen_.message(P_PRINTSTATUS, std::string(name) + " saved, now PADS", 4000);
        }
        else screen_.message(P_PRINTSTATUS, "Error while writing");
        printList();
    }

    void workerMain()
    {
        uint32_t lastSeq = 1;
        int lastRhythm = -1, lastNotes = -1, lastRange = -1, lastKey = -2;
        float lastDens = -1.f;
        genralink::Loop tmp;
        genralink::Link::clearLoop(loop_);
        genralink::Link::Alive alive;
        if (presetRow_.load() < 0) applyPreset(clamp(getInt(P_CAT), 0, kCats - 1), 0); else rememberSound();
        long long lastSlow = 0;
        while (!quit_.load())
        {
            genralink::LinkData* L = link_.data();
            if (resyncReq_.exchange(false)) { screen_.resync(); rememberSound(); presetCat_ = clamp(getInt(P_CAT), 0, kCats - 1); }
            bool on = L && alive.check(L->writerBeat.load(), nowMs());
            bool newLoop = false;
            if (L)
            {
                uint32_t seq = 0;
                if (L->magic.load() == genralink::kMagic && L->loopSeq.load() != lastSeq && link_.readLoop(tmp, seq) && seq != 0)
                {
                    lastSeq = seq;
                    loop_ = tmp;
                    newLoop = true;
                }
            }
            int keyRoot = L ? L->keyRoot.load() : -1;
            bool keyMinor = L ? L->keyMinor.load() != 0 : true;
            int keyCode = keyRoot * 2 + (keyMinor ? 1 : 0);
            int rh = getInt(P_RHYTHM), no = getInt(P_NOTES), ra = getInt(P_RANGE);
            float de = get(P_DENSITY);
            if (newLoop || rh != lastRhythm || no != lastNotes || ra != lastRange || keyCode != lastKey || std::fabs(de - lastDens) > 0.5f)
            {
                lastRhythm = rh; lastNotes = no; lastRange = ra; lastDens = de; lastKey = keyCode;
                Line line;
                buildLine(loop_, keyRoot, keyMinor, line);
                publishLine(line);
            }
            for (auto& t : screen_.taps())
            {
                if (t.first == P_NEW && L) { L->lane[genralink::LANE_MEL].newRequest.fetch_add(1); screen_.message(P_STATUS, on ? "NEW: new melody from the next bar" : "no GENRA CHOP"); }
                else if (t.first == P_UNDO && L) { L->lane[genralink::LANE_MEL].undoRequest.fetch_add(1); screen_.message(P_STATUS, on ? "UNDO: the last melody" : "no GENRA CHOP"); }
                else if (t.first == P_PRINT) print(L);
                else if (t.first == P_CAT) applyPreset(clamp(getInt(P_CAT), 0, kCats - 1), 0);
                else if (t.first >= P_PRESET0 && t.first < P_PRESET0 + kPresetRows) applyPreset(clamp(getInt(P_CAT), 0, kCats - 1), t.first - P_PRESET0);
            }
            if (soundChanged()) { edited_ = true; rememberSound(); }
            if (nowMs() - lastSlow > 1000)
            {
                lastSlow = nowMs();
                printList();
                if (L)
                {
                    int row = presetRow_.load();
                    std::string nm = row >= 0 && row < kPresetRows ? kPresets[presetCat_][row].name : "own sound";
                    genralink::Link::writeName(L->lane[genralink::LANE_MEL], nm);
                }
            }
            texts(L, on);
            lineColumns();
            screen_.push();
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
        }
    }

    genraui::Screen screen_;
    genralink::Link link_;
    bool started_ = false;
    std::thread worker_;
    std::atomic<bool> quit_{false}, resyncReq_{false};
    Line lines_[3];
    Line shown_;
    genralink::Loop loop_;
    std::atomic<int> ready_{-1}, inUse_{-1}, curStep_{-1}, presetRow_{-1};
    int presetCat_ = 1;
    bool edited_ = false;
    float soundSeen_[kSoundN] = {};
    double sr_ = 44100.0;
    mpc::dsp::PolyBlepOsc oscA_, oscB_;
    mpc::dsp::SVF filt_, breath_;
    float ks_[4096] = {};
    int ksLen_ = 0, ksPos_ = 0;
    float ksLp_ = 0.f;
    unsigned ctrl_ = 0;
    uint32_t rng_ = 0x2468ace1u;
    int engine_ = E_EPIANO, stage_ = 0, click_ = 0, clickLen_ = 1;
    float env_ = 0.f, fenv_ = 0.f, menv_ = 0.f, tenv_ = 0.f, vel_ = 1.f, gainSmooth_ = -1.f, noteT_ = 0.f, dull_ = 0.f;
    float ph1_ = 0.f, ph2_ = 0.f, ph3_ = 0.f, ph4_ = 0.f, lfoV_ = 0.f, lfoW_ = 0.f, lfoF_ = 0.f;
    float hz_ = 440.f, targetHz_ = 440.f;
    double gate_ = -1.0, nextStep_ = 0.0;
    bool wasPlaying_ = false;
    Pending pending_[8];
    int nPending_ = 0;
    int heldKey_[8] = {}, heldNote_[8] = {}, nHeld_ = 0;
};

MPC_PLUGIN_ENTRY(GenraMelody)
