// GENRA BASS - the bass companion of GENRA CHOP, on its own MPC track.
//
// SPDX-License-Identifier: GPL-3.0-only
//
// Pages (genra-chop/tools/build_skin.py), on the function keys:
//   SOUND  (opens first) the bass kinds 808 / SUB / UPRIGHT / E-BASS / REESE / ACID, their presets to tap, and the
//          sound: OSC (WAVE, SUB, DETUNE, NOISE), FILTER (CUTOFF, RESO, ENV, DRIVE),
//          ENVELOPE (ATTACK, DECAY, SUSTAIN, RELEASE): one Q-Link row each
//   BASS   the bass line of the loop with GENRA CHOP's chords, NEW, UNDO, AUTO | PADS, KEY LOCK, RHYTHM
//          (Kick / Chop / Own), NOTES (Root / Chord / Free), PATTERN (DENSITY, SWING, GLIDE, OCTAVE),
//          TONE (CUTOFF, DRIVE, DECAY, LEVEL)
//   PRINT  the bass line as a MIDI file; afterwards PADS
//   SETUP  the status
//
// The line comes from GENRA CHOP over LINK v3 (common/genra_link.h): the engine's bass, which this plugin shapes:
// RHYTHM Kick puts the notes on GENRA CHOP's kicks, Chop on its chops, Own keeps the engine's rhythm; NOTES Root
// plays the bar's root, Chord pulls every note onto a chord tone, Free keeps the line; DENSITY thins it out.
// AUTO plays it on the MPC's transport; PADS plays only the notes of this track. KEY LOCK pulls the notes of this
// track into GENRA CHOP's key. NEW / UNDO ask GENRA CHOP for a new bass line / the last one.
//
// Sound: one voice: WAVE Saw / Square / Sine / 808 (sine that falls into the note) / Pluck (a plucked string,
// Karplus-Strong) / Reese (two detuned saws), SUB (sine an octave down), DETUNE (a second oscillator), NOISE
// (the attack's click / thump) -> low-pass (SVF: CUTOFF, RESO, ENV) -> DRIVE -> ADSR -> LEVEL, lifted when the
// filter is closed (up to +9 dB) -> soft limiter.
// MIDI OUT (test, 0.4): the loop's notes also go to an ALSA sequencer port "GENRA BASS OUT" (common/genra_midiout.h),
// so the MPC can record them on a MIDI track whose input is that port. SETUP shows the port and the notes sent.
// Threads: process() (audio); a worker copies the loop out of the shared memory, builds the line and the screen.

#include "mpc_plugin.h"
#include "dsp/oscillator.h"
#include "dsp/svf.h"
#include "../../common/genra_files.h"
#include "../../common/genra_link.h"
#include "../../common/genra_midiout.h"
#include "../../common/genra_ui.h"

#include <chrono>
#include <cmath>
#include <thread>

using namespace mpc;
using namespace genrafiles;

namespace {

static const int kSteps = genralink::kSteps;
static const int kCats = 6, kPresetRows = 6, kPrintRows = 7;
static const int kColFrames = 34; // bass line column: 0 empty, 1..16 a note at that height, +17 the step that plays

const char* const kNotes[12] = {"C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};
const char* const kQual[8] = {"", "m", "7", "maj7", "m7", "m9", "dim", "sus4"};
const int kQualTones[8][5] = {{0, 4, 7, -1, -1}, {0, 3, 7, -1, -1}, {0, 4, 7, 10, -1}, {0, 4, 7, 11, -1},
                              {0, 3, 7, 10, -1}, {0, 3, 7, 10, 2}, {0, 3, 6, -1, -1}, {0, 5, 7, -1, -1}};
const int kMinor[7] = {0, 2, 3, 5, 7, 8, 10};
const int kMajor[7] = {0, 2, 4, 5, 7, 9, 11};
const char* const kCatNames[kCats] = {"808", "SUB", "UPRIGHT", "E-BASS", "REESE", "ACID"};
enum Wave { W_SAW, W_SQUARE, W_SINE, W_808, W_PLUCK, W_REESE, NUM_WAVES };
const char* const kWaveNames[NUM_WAVES] = {"Saw", "Square", "Sine", "808", "Pluck", "Reese"};

// a preset: wave, sub, detune, noise, cutoff, reso, env, drive, attack, decay, sustain, release, glide
struct Preset { const char* name; float v[13]; };
const Preset kPresets[kCats][kPresetRows] = {
    {{"Classic 808", {W_808, 0, 0, 10, 70, 5, 10, 25, 0, 70, 0, 40, 30}},
     {"Long 808", {W_808, 0, 0, 5, 60, 5, 5, 20, 0, 95, 0, 60, 45}},
     {"Punch 808", {W_808, 10, 0, 30, 85, 10, 25, 45, 0, 45, 0, 25, 20}},
     {"Dirty 808", {W_808, 0, 0, 15, 75, 15, 20, 85, 0, 75, 0, 45, 35}},
     {"Slide 808", {W_808, 0, 0, 5, 65, 5, 5, 30, 0, 85, 0, 50, 80}},
     {"Short 808", {W_808, 0, 0, 20, 70, 5, 15, 30, 0, 25, 0, 15, 10}}},
    {{"Deep Sub", {W_SINE, 40, 0, 0, 40, 0, 0, 10, 2, 60, 70, 30, 20}},
     {"Round Sub", {W_SINE, 60, 0, 5, 45, 5, 10, 20, 2, 55, 60, 25, 15}},
     {"Warm Sub", {W_SAW, 70, 0, 0, 22, 10, 15, 15, 3, 60, 60, 30, 15}},
     {"Pulse Sub", {W_SQUARE, 60, 0, 0, 20, 5, 10, 20, 2, 50, 55, 25, 15}},
     {"Lo-Fi Sub", {W_SINE, 50, 0, 15, 35, 0, 5, 45, 3, 55, 50, 30, 10}},
     {"Hold Sub", {W_SINE, 50, 0, 0, 40, 0, 0, 10, 5, 80, 90, 50, 30}}},
    {{"Warm Upright", {W_PLUCK, 40, 0, 35, 40, 5, 30, 10, 0, 55, 0, 30, 0}},
     {"Jazz Walk", {W_PLUCK, 30, 0, 45, 50, 5, 35, 5, 0, 50, 0, 25, 0}},
     {"Dusty Pluck", {W_PLUCK, 35, 0, 50, 35, 10, 25, 30, 0, 45, 0, 20, 0}},
     {"Dilla Upright", {W_PLUCK, 45, 0, 40, 30, 5, 20, 20, 0, 60, 0, 30, 5}},
     {"Muted Thumb", {W_PLUCK, 40, 0, 25, 25, 0, 15, 10, 0, 30, 0, 15, 0}},
     {"Late Night", {W_PLUCK, 50, 0, 30, 28, 5, 20, 15, 1, 70, 0, 40, 10}}},
    {{"Finger Bass", {W_PLUCK, 30, 0, 20, 55, 10, 40, 20, 0, 60, 30, 25, 0}},
     {"P-Bass", {W_SAW, 30, 0, 15, 35, 10, 35, 25, 1, 55, 40, 20, 5}},
     {"Funk Slap", {W_PLUCK, 20, 0, 60, 70, 20, 60, 35, 0, 35, 20, 15, 0}},
     {"G-Funk Whine", {W_SQUARE, 20, 8, 0, 45, 30, 30, 20, 5, 70, 60, 40, 60}},
     {"Muted Pick", {W_SAW, 25, 0, 30, 40, 15, 45, 30, 0, 25, 10, 10, 0}},
     {"Round Flat", {W_SQUARE, 40, 0, 10, 30, 5, 25, 15, 1, 55, 45, 25, 5}}},
    {{"Reese", {W_REESE, 30, 25, 0, 40, 20, 20, 35, 3, 70, 70, 35, 10}},
     {"Dark Reese", {W_REESE, 40, 18, 0, 28, 25, 15, 45, 4, 75, 75, 40, 15}},
     {"Wide Reese", {W_REESE, 20, 40, 5, 50, 15, 25, 30, 3, 70, 70, 35, 10}},
     {"Drill Reese", {W_REESE, 50, 22, 0, 35, 30, 35, 60, 2, 60, 55, 30, 40}},
     {"Growl", {W_REESE, 20, 30, 10, 45, 50, 45, 75, 2, 60, 60, 25, 10}},
     {"Soft Reese", {W_REESE, 50, 12, 0, 25, 5, 10, 15, 6, 80, 80, 45, 20}}},
    {{"303 Squelch", {W_SAW, 0, 0, 0, 30, 80, 75, 40, 0, 35, 10, 15, 40}},
     {"Acid Square", {W_SQUARE, 0, 0, 0, 25, 75, 70, 35, 0, 35, 10, 15, 40}},
     {"Rubber", {W_SAW, 10, 0, 0, 35, 60, 55, 30, 0, 45, 20, 20, 50}},
     {"Screamer", {W_SAW, 0, 0, 0, 40, 90, 85, 70, 0, 30, 10, 10, 30}},
     {"Bubble", {W_SQUARE, 10, 0, 0, 20, 70, 60, 25, 0, 40, 15, 20, 60}},
     {"Deep Acid", {W_SAW, 30, 0, 0, 20, 65, 50, 35, 0, 50, 25, 20, 45}}},
};

static const char* const kCaptions[][2] = {
    {"genra", "GENRA"}, {"name", "BASS"}, {"new", "NEW"}, {"undo", "UNDO"}, {"auto", "AUTO"}, {"padsmode", "PADS"},
    {"keylock", "KEY LOCK"}, {"rhythm", "RHYTHM"}, {"rh1", "KICK"}, {"rh2", "CHOP"}, {"rh3", "OWN"},
    {"notes", "NOTES"}, {"no1", "ROOT"}, {"no2", "CHORD"}, {"no3", "FREE"}, {"pattern", "PATTERN"},
    {"tone", "TONE"}, {"osc", "OSC"}, {"filter", "FILTER"}, {"env", "ENVELOPE"},
    {"cat1", "808"}, {"cat2", "SUB"}, {"cat3", "UPRIGHT"}, {"cat4", "E-BASS"}, {"cat5", "REESE"},
    {"cat6", "ACID"}, {"hint", "Tap a preset to load · double tap = big knob"},
    {"print", "PRINT"}, {"printtitle", "· PRINT"}, {"after", "After PRINT"}, {"autoinfo", "AUTO: the loop plays"},
    {"padsinfo", "PADS: only the MPC track's notes"}, {"r1", "LINK"}, {"r2", "Key"}, {"r3", "Print folder"},
    {"r4", "MIDI OUT"}};
static const int kNumCaps = (int)(sizeof(kCaptions) / sizeof(kCaptions[0]));

enum
{
    P_WAVE = 0, P_SUB, P_DETUNE, P_NOISE, P_CUTOFF, P_RESO, P_ENV, P_DRIVE, P_ATTACK, P_DECAY, P_SUSTAIN, P_RELEASE,
    P_DENSITY, P_SWING, P_GLIDE, P_OCT, P_LEVEL,
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

} // namespace

class GenraBass : public Plugin
{
public:
    GenraBass() : Plugin(makeInfo()), screen_(*this)
    {
        std::vector<std::string> waves(kWaveNames, kWaveNames + NUM_WAVES);
        addParam(choiceParam("wave", "WAVE", waves, W_PLUCK));
        addParam(floatParam("sub", "SUB", 0.f, 100.f, 40.f, "%", 0));
        addParam(floatParam("detune", "DETUNE", 0.f, 50.f, 0.f, "ct", 0));
        addParam(floatParam("noise", "NOISE", 0.f, 100.f, 35.f, "%", 0));
        addParam(floatParam("cutoff", "CUTOFF", 0.f, 100.f, 40.f, "", 0));
        addParam(floatParam("reso", "RESO", 0.f, 100.f, 5.f, "%", 0));
        addParam(floatParam("env", "ENV", 0.f, 100.f, 30.f, "%", 0));
        addParam(floatParam("drive", "DRIVE", 0.f, 100.f, 10.f, "%", 0));
        addParam(floatParam("attack", "ATTACK", 0.f, 100.f, 0.f, "", 0));
        addParam(floatParam("decay", "DECAY", 0.f, 100.f, 55.f, "%", 0));
        addParam(floatParam("sustain", "SUSTAIN", 0.f, 100.f, 0.f, "%", 0));
        addParam(floatParam("release", "RELEASE", 0.f, 100.f, 30.f, "", 0));
        addParam(floatParam("density", "DENSITY", 0.f, 100.f, 100.f, "%", 0));
        addParam(floatParam("swing", "SWING", -25.f, 25.f, 0.f, "%", 0));
        addParam(floatParam("glide", "GLIDE", 0.f, 100.f, 0.f, "", 0));
        addParam(intParam("oct", "OCTAVE", -2, 2, 0));
        addParam(floatParam("level", "LEVEL", -40.f, 6.f, -6.f, "dB", 1));
        addParam(choiceParam("cat", "KIND", {kCatNames[0], kCatNames[1], kCatNames[2], kCatNames[3], kCatNames[4], kCatNames[5]}, 2));
        addParam(choiceParam("auto", "AUTO PADS", {"Auto", "Pads"}, 0));
        addParam(boolParam("keylock", "KEY LOCK", true));
        addParam(choiceParam("rhythm", "RHYTHM", {"Kick", "Chop", "Own"}, 2));
        addParam(choiceParam("notes", "NOTES", {"Root", "Chord", "Free"}, 2));
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
        screen_.setText(P_VERSION, "opening ...");
    }

    ~GenraBass() override
    {
        quit_.store(true);
        if (worker_.joinable()) worker_.join();
    }

    static PluginInfo makeInfo()
    {
        PluginInfo i;
        i.name = "GENRA BASS";
        i.vendor = "Genra";
        i.uniqueId = MPC_FOURCC('G', 'n', 'B', 's');
        i.version = 400;
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
        sub_.setSampleRate(sampleRate);
        sine_.setSampleRate(sampleRate);
        filt_.setSampleRate(sampleRate);
        if (!started_)
        {
            started_ = true;
            link_.open();
            worker_ = std::thread([this] { workerMain(); });
        }
    }

    void reset() override
    {
        env_ = 0.f; fenv_ = 0.f; gainSmooth_ = -1.f; filt_.reset();
        gate_ = -1.0; wasPlaying_ = false; nPending_ = 0; stage_ = 0;
        for (float& k : ks_) k = 0.f;
    }

    void process(ProcessContext& ctx) override
    {
        genralink::LinkData* L = link_.data();
        if (L) L->lane[genralink::LANE_BASS].beat.fetch_add(1, std::memory_order_relaxed);
        const bool keyLock = getBool(P_KEYLOCK);
        const bool autoMode = getInt(P_AUTO) == 0;
        const int oct = getInt(P_OCT);
        wave_ = clamp(getInt(P_WAVE), 0, NUM_WAVES - 1);

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
                const Note& sn = line->n[s];
                if (sn.pitch >= 0 && nPending_ < 8)
                {
                    double at = (nextStep_ - q0) * spStep;
                    if (s & 1) at += (swing - 0.5f) * 2.f * spStep;
                    if (sn.off > 0.f) at += sn.off * spStep;
                    Pending& p = pending_[nPending_++];
                    p.at = at;
                    p.note = clampNote(sn.pitch + 12 * oct);
                    p.vel = sn.vel * (sn.acc ? 1.f : 0.85f);
                    p.slide = sn.slide;
                    p.gate = std::max(0.2, (double)sn.len) * spStep;
                }
                curStep_.store(s, std::memory_order_relaxed);
                nextStep_ += 1.0;
            }
            wasPlaying_ = true;
        }
        else
        {
            if (wasPlaying_ && gate_ > 0.0) gate_ = 0.0;
            if (wasPlaying_) { midi_.allOff(); midiNote_ = -1; midiGate_ = 0.0; }
            wasPlaying_ = false;
            nPending_ = 0;
            curStep_.store(playing ? curStepOf(tr) : -1, std::memory_order_relaxed);
        }

        const float cutoff = get(P_CUTOFF) / 100.f, reso = get(P_RESO) / 100.f, envAmt = get(P_ENV) / 100.f;
        // level compensation: the more the low-pass is closed, the more it is lifted (up to +9 dB)
        const float comp = std::pow(10.f, 9.f * std::pow(1.f - cutoff, 1.5f) / 20.f);
        float target = std::pow(10.f, get(P_LEVEL) / 20.f) * comp;
        if (gainSmooth_ < 0.f) gainSmooth_ = target;
        const float drive = 1.f + 5.f * get(P_DRIVE) / 100.f;
        const float att = 0.0005f + 0.5f * std::pow(get(P_ATTACK) / 100.f, 2.f);
        const float dec = 0.04f + 2.5f * std::pow(get(P_DECAY) / 100.f, 2.f);
        const float sus = get(P_SUSTAIN) / 100.f;
        const float rel = 0.01f + 1.5f * std::pow(get(P_RELEASE) / 100.f, 2.f);
        const float subLvl = get(P_SUB) / 100.f, noiseLvl = get(P_NOISE) / 100.f, detune = get(P_DETUNE);
        const float attC = 1.f / (float)(sr_ * att);
        const float decC = std::exp(-1.f / (float)(sr_ * dec));
        const float relC = std::exp(-1.f / (float)(sr_ * rel));
        const float fdec = std::exp(-1.f / (float)(sr_ * (0.03 + 0.4 * dec)));
        const float glideT = 0.004f + 0.3f * get(P_GLIDE) / 100.f;
        const float glideC = 1.f - std::exp(-1.f / (float)(sr_ * glideT));
        const float ksDamp = 0.35f + 0.6f * cutoff;
        const float ksFeedback = 0.990f + 0.0095f * (get(P_DECAY) / 100.f);
        const float driveNorm = std::tanh(drive);
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
                    int note = keyLock ? snapToKey(e.note(), L) : e.note();
                    noteOn(clampNote(note + 12 * oct), e.velocity() / 127.f, false, -1.0);
                    heldNote_ = e.note();
                }
                else if ((e.isNoteOff() && e.note() == heldNote_) || e.isAllNotesOff()) gate_ = 0.0;
            }
            for (int k = 0; k < nPending_;)
            {
                if (pending_[k].at <= (double)i)
                {
                    const Pending& p = pending_[k];
                    noteOn(p.note, p.vel, p.slide, p.gate);
                    // the same note to the MIDI port: a slide overlaps the old note (legato), else it ends first
                    const int v = std::max(1, (int)std::lround(p.vel * 127.f));
                    if (p.slide && midiNote_ >= 0 && midiNote_ != p.note) { midi_.noteOn(p.note, v); midi_.noteOff(midiNote_); }
                    else { if (midiNote_ >= 0) midi_.noteOff(midiNote_); midi_.noteOn(p.note, v); }
                    midiNote_ = p.note;
                    midiGate_ = p.gate;
                    pending_[k] = pending_[--nPending_];
                }
                else ++k;
            }
            gainSmooth_ += (target - gainSmooth_) * 0.002f;
            if (gate_ > 0.0) { gate_ -= 1.0; if (gate_ < 0.0) gate_ = 0.0; }
            if (midiNote_ >= 0 && midiGate_ > 0.0) { midiGate_ -= 1.0; if (midiGate_ <= 0.0) { midi_.noteOff(midiNote_); midiNote_ = -1; } }
            const bool held = gate_ < 0.0 || gate_ > 0.0;
            if (!held && stage_ != 0) stage_ = 0;
            if (stage_ == 1) { env_ += attC; if (env_ >= 1.f) { env_ = 1.f; stage_ = 2; } }
            else if (stage_ == 2) env_ = sus + (env_ - sus) * decC;
            else env_ *= relC;
            fenv_ *= fdec;
            hz_ += (targetHz_ - hz_) * glideC;
            if ((ctrl_++ & 15) == 0)
            {
                float f = hz_;
                if (wave_ == W_808) { f *= 1.f + 1.5f * drop_; drop_ *= 0.94f; }
                float cents = std::pow(2.f, detune / 1200.f);
                oscA_.setWave(wave_ == W_SQUARE ? mpc::dsp::Wave::Square : mpc::dsp::Wave::Saw);
                oscB_.setWave(wave_ == W_SQUARE ? mpc::dsp::Wave::Square : mpc::dsp::Wave::Saw);
                oscA_.setFrequency(wave_ == W_REESE ? f * cents : f);
                oscB_.setFrequency(wave_ == W_REESE ? f / cents : f * cents);
                sine_.setFrequency(f);
                sub_.setFrequency(f * 0.5f);
                float cut = 40.f * std::pow(2.f, cutoff * 8.f) * (1.f + 6.f * envAmt * fenv_ * vel_);
                filt_.set(std::fmin(cut, 14000.f), 0.05f + 0.9f * reso);
            }
            float raw = 0.f;
            switch (wave_)
            {
                case W_SAW: case W_SQUARE: raw = 0.6f * oscA_.process() + (detune > 0.5f ? 0.4f * oscB_.process() : 0.f); break;
                case W_SINE: case W_808: raw = sine_.process(); break;
                case W_REESE: raw = 0.5f * oscA_.process() + 0.5f * oscB_.process(); break;
                case W_PLUCK: raw = pluck(ksDamp, ksFeedback); break;
                default: break;
            }
            raw += subLvl * 0.8f * sub_.process();
            if (click_ > 0) { raw += noiseLvl * 0.6f * noise() * ((float)click_ / (float)clickLen_); --click_; }
            float y = filt_.process(raw, mpc::dsp::SVF::Mode::Lowpass);
            y = std::tanh(y * drive) / driveNorm;
            y *= env_ * vel_ * gainSmooth_ * 0.8f;
            y = std::tanh(y * 1.1f) / 1.1f; // soft limiter
            outL[i] = y;
            if (outR) outR[i] = y;
        }
        for (int k = 0; k < nPending_; ++k) pending_[k].at -= ctx.numFrames;
        inUse_.store(-1, std::memory_order_release);
    }

    void saveExtraState(std::vector<uint8_t>& out) override
    {
        std::string s = "GB2\n" + std::to_string(presetRow_.load()) + "\n";
        out.assign(s.begin(), s.end());
    }
    void loadExtraState(const uint8_t* data, size_t size) override
    {
        std::string s(reinterpret_cast<const char*>(data), size);
        if (s.compare(0, 4, "GB2\n") == 0) presetRow_.store(std::atoi(s.c_str() + 4));
        resyncReq_.store(true);
    }

    bool formatValue(int index, float plain, std::string& out) override
    {
        char b[32];
        switch (index)
        {
            case P_CUTOFF:
            {
                float hz = 40.f * std::pow(2.f, plain / 100.f * 8.f);
                if (hz >= 1000.f) std::snprintf(b, sizeof b, "%.1fk", hz / 1000.f);
                else std::snprintf(b, sizeof b, "%.0f", hz);
                out = b;
                return true;
            }
            case P_ATTACK: std::snprintf(b, sizeof b, "%.0f ms", (0.0005f + 0.5f * std::pow(plain / 100.f, 2.f)) * 1000.f); out = b; return true;
            case P_RELEASE: std::snprintf(b, sizeof b, "%.0f ms", (0.01f + 1.5f * std::pow(plain / 100.f, 2.f)) * 1000.f); out = b; return true;
            case P_GLIDE: std::snprintf(b, sizeof b, "%.0f ms", (0.004f + 0.3f * plain / 100.f) * 1000.f); out = b; return true;
            case P_SWING:
                if (std::fabs(plain) < 0.5f) out = "from chop";
                else { std::snprintf(b, sizeof b, "%+.0f %%", plain); out = b; }
                return true;
            case P_OCT: std::snprintf(b, sizeof b, "%+d", (int)std::lround(plain)); out = b; return true;
            case P_DETUNE: std::snprintf(b, sizeof b, "%.0f ct", plain); out = b; return true;
            case P_LEVEL: std::snprintf(b, sizeof b, "%+.1f dB", plain); out = b; return true;
            case P_SUB: case P_NOISE: case P_RESO: case P_ENV: case P_DRIVE: case P_DECAY: case P_SUSTAIN: case P_DENSITY:
                std::snprintf(b, sizeof b, "%.0f %%", plain); out = b; return true;
            default: break;
        }
        if (index >= P_COL0) return false;
        return screen_.format(index, out);
    }

private:
    struct Pending { double at; int note; float vel; bool slide; double gate; };

    static float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
    static int clampNote(int n) { return n < 12 ? 12 : (n > 96 ? 96 : n); }
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

    // gate: samples the note is held (loop), -1: until note-off (keys)
    void noteOn(int note, float vel, bool slide, double gate)
    {
        float hz = (float)(440.0 * std::pow(2.0, (note - 69) / 12.0));
        targetHz_ = hz;
        const bool legato = slide && env_ > 0.05f;
        if (!legato)
        {
            hz_ = hz;
            fenv_ = 1.f;
            stage_ = 1;
            drop_ = 1.f;
            vel_ = 0.3f + 0.7f * vel;
            click_ = clickLen_ = std::max(1, (int)(sr_ * 0.006));
            if (wave_ == W_PLUCK)
            {
                ksLen_ = std::max(2, std::min((int)(sizeof(ks_) / sizeof(ks_[0])), (int)std::lround(sr_ / hz)));
                float lp = 0.f;
                for (int k = 0; k < ksLen_; ++k) { lp += 0.5f * (noise() - lp); ks_[k] = lp; }
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

    void buildLine(const genralink::Loop& l, Line& out)
    {
        const int rhythm = getInt(P_RHYTHM), notes = getInt(P_NOTES);
        const float dens = get(P_DENSITY) / 100.f;
        const int densLimit = dens >= 0.999f ? 256 : (int)std::lround(dens * 254.f);
        out = Line();
        out.swing = l.swingX1000 / 1000.f;
        out.valid = true;
        int lastPitch = -1;
        int pitchAt[kSteps];
        for (int s = 0; s < kSteps; ++s) { if (l.bass[s].pitch >= 0) lastPitch = l.bass[s].pitch; pitchAt[s] = lastPitch; }
        if (lastPitch >= 0) for (int s = 0; s < kSteps && pitchAt[s] < 0; ++s) pitchAt[s] = lastPitch;
        for (int s = 0; s < kSteps; ++s)
        {
            const int bar = s / 16;
            Note n;
            if (rhythm == 2)
            {
                const genralink::StepNote& b = l.bass[s];
                if (b.pitch < 0 || (int)b.prio > densLimit) continue;
                n.pitch = b.pitch; n.len = b.len16 / 16.f; n.vel = b.vel / 127.f; n.off = b.off / 100.f;
                n.slide = (b.flags & 2) != 0; n.acc = (b.flags & 1) != 0;
            }
            else
            {
                auto hitAt = [&](int k) { return rhythm == 0 ? l.drum[0][k].vel > 0 : l.chop[k] != 0; };
                if (!hitAt(s) || pitchAt[s] < 0) continue;
                uint32_t h = (uint32_t)s * 2654435761u ^ l.id * 40503u;
                if (dens < 0.999f && ((h >> 8) & 255) / 255.f > dens) continue;
                int next = s + 1;
                while (next < kSteps && !hitAt(next)) ++next;
                n.pitch = (int8_t)pitchAt[s];
                n.len = (float)std::min(4, next - s);
                n.vel = rhythm == 0 ? l.drum[0][s].vel / 127.f : 0.85f;
                n.off = rhythm == 0 ? l.drum[0][s].off / 100.f : 0.f;
            }
            const int root = l.barRoot[bar], q = l.barQuality[bar];
            if (notes == 0 && root >= 0)
            {
                int p = n.pitch;
                int cand = (p / 12) * 12 + root;
                if (cand - p > 6) cand -= 12;
                if (p - cand > 6) cand += 12;
                n.pitch = (int8_t)cand;
            }
            else if (notes == 1) n.pitch = (int8_t)nearestChordTone(n.pitch, root, q);
            out.n[s] = n;
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
        const char* rh[3] = {"bass on the kick", "bass on the chops", "the engine's rhythm"};
        const int rhythm = clamp(getInt(P_RHYTHM), 0, 2);
        int step = curStep_.load();
        if (step >= 0)
        {
            int bar = step / 16;
            std::snprintf(b, sizeof b, "Bar %d · %s · %s", bar + 1, chordName(loop_.barRoot[bar], loop_.barQuality[bar]).c_str(), rh[rhythm]);
            screen_.setText(P_STATUS, b);
        }
        else screen_.setText(P_STATUS, std::string(getInt(P_AUTO) == 0 ? "AUTO · " : "PADS · ") + rh[rhythm]);
        std::snprintf(b, sizeof b, "Bass line · 4 bars · %.0f BPM · %s", bpm100 / 100.0, key.c_str());
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
        for (int k = 0; k < 12; ++k) { set(P_WAVE + k, p.v[k]); notifyHostParameterChanged(P_WAVE + k); }
        set(P_GLIDE, p.v[12]);
        notifyHostParameterChanged(P_GLIDE);
        presetRow_.store(row);
        presetCat_ = cat;
        rememberSound();
        edited_ = false;
        if (genralink::LinkData* L = link_.data()) genralink::Link::writeName(L->lane[genralink::LANE_BASS], p.name);
    }
    int soundParam(int k) const { return k == 12 ? P_GLIDE : P_WAVE + k; }
    void rememberSound() { for (int k = 0; k < 13; ++k) soundSeen_[k] = get(soundParam(k)); }
    bool soundChanged() const
    {
        for (int k = 0; k < 13; ++k) if (std::fabs(get(soundParam(k)) - soundSeen_[k]) > 1e-3f) return true;
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
            notes.push_back({at, std::max(0.2, (double)n.len), clampNote(n.pitch + 12 * oct), (int)std::lround(n.vel * 127.f)});
        }
        int bpm100 = L ? L->bpmX100.load() : 0;
        double bpm = bpm100 > 0 ? bpm100 / 100.0 : 120.0;
        int no = printNumberFor(dir, loop_.id);
        char name[96];
        std::snprintf(name, sizeof name, "Print %02d Bass %s %.0f", no, keyOf(L).c_str(), bpm);
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
        int lastRhythm = -1, lastNotes = -1;
        float lastDens = -1.f;
        genralink::Loop tmp;
        genralink::Link::clearLoop(loop_);
        genralink::Link::Alive alive;
        midi_.open("GENRA BASS");
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
            int rh = getInt(P_RHYTHM), no = getInt(P_NOTES);
            float de = get(P_DENSITY);
            if (newLoop || rh != lastRhythm || no != lastNotes || std::fabs(de - lastDens) > 0.5f)
            {
                lastRhythm = rh; lastNotes = no; lastDens = de;
                Line line;
                buildLine(loop_, line);
                publishLine(line);
            }
            for (auto& t : screen_.taps())
            {
                if (t.first == P_NEW && L) { L->lane[genralink::LANE_BASS].newRequest.fetch_add(1); screen_.message(P_STATUS, on ? "NEW: new bass line from the next bar" : "no GENRA CHOP"); }
                else if (t.first == P_UNDO && L) { L->lane[genralink::LANE_BASS].undoRequest.fetch_add(1); screen_.message(P_STATUS, on ? "UNDO: the last bass line" : "no GENRA CHOP"); }
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
                    genralink::Link::writeName(L->lane[genralink::LANE_BASS], nm);
                }
            }
            texts(L, on);
            {
                char mb[48];
                if (midi_.ok()) std::snprintf(mb, sizeof mb, "%s · %ld", midi_.status().c_str(), midi_.sent());
                else std::snprintf(mb, sizeof mb, "off: %s", midi_.status().c_str());
                screen_.setText(P_VERSION, mb);
            }
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
    int presetCat_ = 2;
    bool edited_ = false;
    float soundSeen_[13] = {};
    double sr_ = 44100.0;
    mpc::dsp::PolyBlepOsc oscA_, oscB_;
    mpc::dsp::WavetableSine sub_, sine_;
    mpc::dsp::SVF filt_;
    float ks_[4096] = {};
    int ksLen_ = 0, ksPos_ = 0;
    float ksLp_ = 0.f;
    unsigned ctrl_ = 0;
    uint32_t rng_ = 0x12345678u;
    int wave_ = W_PLUCK, stage_ = 0, click_ = 0, clickLen_ = 1;
    float env_ = 0.f, fenv_ = 0.f, vel_ = 1.f, gainSmooth_ = -1.f, drop_ = 0.f;
    float hz_ = 55.f, targetHz_ = 55.f;
    double gate_ = -1.0, nextStep_ = 0.0;
    genramidi::Out midi_;
    int midiNote_ = -1;
    double midiGate_ = 0.0;
    bool wasPlaying_ = false;
    Pending pending_[8];
    int nPending_ = 0;
    int heldNote_ = -1;
};

MPC_PLUGIN_ENTRY(GenraBass)
