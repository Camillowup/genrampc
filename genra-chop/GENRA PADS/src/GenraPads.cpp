// GENRA PADS - the pad companion of GENRA CHOP, on its own MPC track (6 voices, stereo).
//
// SPDX-License-Identifier: GPL-3.0-only
//
// Pages (genra-chop/tools/build_skin.py), on the function keys:
//   PADS   AUTO: the chords of the loop on the display; PADS: the display shows the chords of the 16 MPC pads.
//          NEW, UNDO, AUTO | PADS, KEY LOCK, RHYTHM (Hold / Stabs / Chop), VOICING (Close / Open / 9-11),
//          PAD SET (Song / Degree / Tension), PATTERN (DENSITY, SPREAD, STRUM, OCTAVE),
//          TONE (CUTOFF, ATTACK, RELEASE, LEVEL)
//   SOUND  the kinds STRINGS / CHOIR / SYNTH / ORGAN / E-PIANO / VINYL, their presets to tap, and the sound:
//          OSC (TONE, ENSEMBLE, DETUNE, AGE), FILTER (CUTOFF, RESO, ENV, DRIVE), ENVELOPE (ATTACK, DECAY, SUSTAIN,
//          RELEASE): one Q-Link row each
//   PRINT  the chords as a MIDI file; afterwards PADS
//   SETUP  the status
//
// AUTO: the chords come from GENRA CHOP over LINK v3 (common/genra_link.h). RHYTHM Hold plays them as they are,
// Stabs plays short hits on a fixed pattern, Chop hits them on GENRA CHOP's chops; DENSITY thins the hits out.
// VOICING Close packs a chord into one octave, Open drops its second voice an octave (drop 2), 9-11 adds the 9th and
// the 11th of the bar's root. SPREAD moves the lowest voice down and the highest up, STRUM plays the voices one
// after the other (0..80 ms).
// PADS: every MPC pad plays a whole chord (the learned pad map of the GENRA plugins, else note 36 = pad 1). PAD SET
// Song takes the chords of the loop and of the chops, Degree the seven chords of the key (with 7th, then with 9th),
// Tension borrowed and dominant chords. KEY LOCK on: the pad chords follow GENRA CHOP's key; off: they stay.
// NEW / UNDO ask GENRA CHOP for new pads / the last ones.
//
// Sound, per voice: TONE Strings (three detuned saws), Choir Ah / Choir Oo (two saws through two formant filters),
// Supersaw, Soft (triangle and sine), Organ (four drawbar sines), E-Piano (FM) -> two low-pass filters in a row
// (24 dB, key tracking, ENV follows the voice's envelope) -> DRIVE -> ADSR. Then ENSEMBLE (a three-tap chorus, the
// stereo), AGE (wow, flutter, hiss, crackle, dull top, above 60 % fewer bits), level compensation (a closed filter
// is lifted up to +15 dB) and a soft limiter.
// Threads: process() (audio); a worker copies the loop out of the shared memory, builds the chords and the screen.

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
static const int kCats = 6, kPresetRows = 6, kPrintRows = 7, kVoices = 6, kMaxNotes = 6, kPads = 16;
static const int kColFrames = 20; // chord column: voices * 4 + 2 (a chord starts) + 1 (the step that plays)

const char* const kNotes[12] = {"C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};
const int kMinor[7] = {0, 2, 3, 5, 7, 8, 10};
const int kMajor[7] = {0, 2, 4, 5, 7, 9, 11};
const char* const kCatNames[kCats] = {"STRINGS", "CHOIR", "SYNTH", "ORGAN", "E-PIANO", "VINYL"};
enum Engine { E_STRINGS, E_CHOIR_A, E_CHOIR_O, E_SUPERSAW, E_SOFT, E_ORGAN, E_EPIANO, NUM_ENGINES };
const char* const kEngineNames[NUM_ENGINES] = {"Strings", "Choir Ah", "Choir Oo", "Supersaw", "Soft", "Organ", "E-Piano"};

// chord types
struct ChordType { const char* suffix; int iv[6]; int n; };
enum CT { C_MAJ, C_MIN, C_7, C_MAJ7, C_M7, C_M9, C_DIM, C_SUS4, C_M7B5, C_DIM7, C_MAJ9, C_9, C_7B9, C_M11, C_MAJ7S11,
          C_7SUS4, C_69, C_M6, C_ADD9, NUM_CT };
const ChordType kCT[NUM_CT] = {
    {"", {0, 4, 7}, 3}, {"m", {0, 3, 7}, 3}, {"7", {0, 4, 7, 10}, 4}, {"maj7", {0, 4, 7, 11}, 4},
    {"m7", {0, 3, 7, 10}, 4}, {"m9", {0, 3, 7, 10, 14}, 5}, {"dim", {0, 3, 6}, 3}, {"sus4", {0, 5, 7}, 3},
    {"m7b5", {0, 3, 6, 10}, 4}, {"dim7", {0, 3, 6, 9}, 4}, {"maj9", {0, 4, 7, 11, 14}, 5}, {"9", {0, 4, 7, 10, 14}, 5},
    {"7b9", {0, 4, 7, 10, 13}, 5}, {"m11", {0, 3, 7, 10, 14, 17}, 6}, {"maj7#11", {0, 4, 7, 11, 18}, 5},
    {"7sus4", {0, 5, 7, 10}, 4}, {"6/9", {0, 4, 7, 9, 14}, 5}, {"m6", {0, 3, 7, 9}, 4}, {"add9", {0, 4, 7, 14}, 4}};
// LINK's bar qualities ("", m, 7, maj7, m7, m9, dim, sus4) as chord types
const int kLinkQ[8] = {C_MAJ, C_MIN, C_7, C_MAJ7, C_M7, C_M9, C_DIM, C_SUS4};
struct Deg { int rel; int ct; };
// DEGREE: the seven chords of the key with 7th (pads 1-7), the tonic an octave up (8), with 9th (9-15), V7sus4 (16)
const Deg kDegMinor[kPads] = {{0, C_M7}, {2, C_M7B5}, {3, C_MAJ7}, {5, C_M7}, {7, C_M7}, {8, C_MAJ7}, {10, C_7},
                              {12, C_M7}, {0, C_M9}, {2, C_M7B5}, {3, C_MAJ9}, {5, C_M9}, {7, C_M9}, {8, C_MAJ9},
                              {10, C_9}, {7, C_7SUS4}};
const Deg kDegMajor[kPads] = {{0, C_MAJ7}, {2, C_M7}, {4, C_M7}, {5, C_MAJ7}, {7, C_7}, {9, C_M7}, {11, C_M7B5},
                              {12, C_MAJ7}, {0, C_MAJ9}, {2, C_M9}, {4, C_M7}, {5, C_MAJ9}, {7, C_9}, {9, C_M9},
                              {11, C_M7B5}, {7, C_7SUS4}};
// TENSION: borrowed, dominant and colour chords
const Deg kTenMinor[kPads] = {{0, C_M11}, {5, C_9}, {8, C_MAJ7S11}, {1, C_MAJ7}, {7, C_7B9}, {11, C_DIM7},
                              {10, C_7SUS4}, {3, C_69}, {0, C_M6}, {5, C_M6}, {2, C_7}, {9, C_M7B5}, {4, C_7},
                              {10, C_9}, {8, C_7}, {6, C_MAJ7}};
const Deg kTenMajor[kPads] = {{0, C_MAJ9}, {0, C_69}, {5, C_MAJ7S11}, {5, C_M6}, {7, C_7SUS4}, {7, C_7B9}, {2, C_7},
                              {4, C_7}, {9, C_9}, {10, C_9}, {8, C_MAJ7}, {3, C_MAJ7}, {1, C_7}, {11, C_M7B5},
                              {2, C_M9}, {9, C_M11}};

// a preset: tone, ensemble, detune, age, cutoff, reso, env, drive, attack, decay, sustain, release
static const int kSoundN = 12;
struct Preset { const char* name; float v[kSoundN]; };
const Preset kPresets[kCats][kPresetRows] = {
    {{"Velvet Strings", {E_STRINGS, 60, 14, 20, 45, 5, 20, 10, 35, 60, 85, 50}},
     {"70s Soul Strings", {E_STRINGS, 75, 18, 30, 55, 5, 15, 15, 25, 60, 85, 45}},
     {"Dark Cinema", {E_STRINGS, 55, 12, 15, 31, 10, 25, 10, 50, 70, 90, 65}},
     {"Tape Ensemble", {E_STRINGS, 85, 20, 55, 40, 5, 15, 15, 30, 60, 85, 50}},
     {"Bright Section", {E_STRINGS, 50, 10, 10, 68, 5, 20, 15, 18, 55, 80, 40}},
     {"Lo-Fi Haze", {E_STRINGS, 70, 16, 70, 32, 5, 10, 20, 40, 60, 85, 60}}},
    {{"Soul Choir", {E_CHOIR_A, 60, 10, 20, 55, 5, 10, 10, 35, 60, 90, 55}},
     {"Ghost Oo", {E_CHOIR_O, 70, 12, 30, 40, 5, 10, 5, 45, 60, 90, 65}},
     {"Gospel Pad", {E_CHOIR_A, 55, 8, 15, 62, 5, 15, 15, 25, 60, 85, 45}},
     {"Dusty Voices", {E_CHOIR_A, 70, 12, 60, 42, 5, 10, 15, 35, 60, 85, 55}},
     {"Angel Air", {E_CHOIR_O, 80, 14, 10, 60, 5, 15, 5, 55, 65, 90, 70}},
     {"Low Monks", {E_CHOIR_O, 50, 6, 20, 34, 10, 10, 15, 40, 60, 90, 60}}},
    {{"Warm Poly", {E_SUPERSAW, 50, 10, 20, 38, 10, 30, 15, 20, 55, 75, 40}},
     {"Juno Dream", {E_SUPERSAW, 80, 12, 25, 48, 10, 25, 10, 30, 60, 80, 50}},
     {"Glass Pad", {E_SOFT, 60, 8, 15, 60, 5, 15, 5, 30, 60, 85, 55}},
     {"Wide Saw", {E_SUPERSAW, 70, 25, 15, 55, 5, 20, 15, 15, 55, 80, 45}},
     {"Night Drive", {E_SUPERSAW, 60, 16, 30, 30, 15, 35, 25, 25, 60, 75, 50}},
     {"Soft Analog", {E_SOFT, 50, 10, 25, 45, 10, 20, 10, 25, 60, 85, 50}}},
    {{"Church Air", {E_ORGAN, 60, 4, 15, 55, 5, 5, 10, 20, 60, 95, 45}},
     {"Soul Organ", {E_ORGAN, 75, 6, 25, 62, 5, 10, 30, 5, 60, 95, 25}},
     {"Soft Drawbar", {E_ORGAN, 50, 3, 10, 40, 5, 5, 5, 15, 60, 95, 35}},
     {"Tape Organ", {E_ORGAN, 70, 6, 60, 45, 5, 5, 20, 10, 60, 95, 35}},
     {"Gospel Sunday", {E_ORGAN, 85, 5, 15, 70, 5, 10, 35, 5, 60, 95, 30}},
     {"Dark Organ", {E_ORGAN, 60, 4, 20, 36, 10, 5, 20, 15, 60, 95, 40}}},
    {{"Rhodes Pad", {E_EPIANO, 55, 6, 20, 50, 5, 15, 15, 10, 70, 60, 55}},
     {"Dream Keys", {E_EPIANO, 75, 10, 15, 55, 5, 15, 10, 25, 70, 70, 65}},
     {"Suitcase Chords", {E_EPIANO, 65, 6, 35, 45, 5, 20, 20, 3, 65, 50, 45}},
     {"Lo-Fi Keys", {E_EPIANO, 60, 8, 65, 38, 5, 15, 20, 5, 65, 50, 50}},
     {"Bright Tines", {E_EPIANO, 50, 4, 10, 68, 5, 25, 15, 2, 60, 45, 40}},
     {"Velvet Keys", {E_EPIANO, 60, 8, 25, 35, 5, 10, 10, 8, 70, 60, 55}}},
    {{"Dusty Strings", {E_STRINGS, 70, 16, 80, 35, 5, 10, 20, 30, 60, 85, 55}},
     {"Crackle Choir", {E_CHOIR_A, 65, 12, 85, 40, 5, 10, 15, 35, 60, 85, 55}},
     {"Old Record", {E_SOFT, 60, 10, 90, 38, 5, 10, 20, 25, 60, 85, 50}},
     {"Broken Organ", {E_ORGAN, 65, 8, 85, 42, 5, 5, 25, 10, 60, 95, 35}},
     {"Warm Static", {E_SUPERSAW, 60, 12, 75, 30, 10, 20, 20, 25, 60, 80, 50}},
     {"Sample Haze", {E_EPIANO, 70, 10, 80, 36, 5, 10, 20, 10, 65, 60, 55}}},
};

static const char* const kCaptions[][2] = {
    {"genra", "GENRA"}, {"name", "PADS"}, {"new", "NEW"}, {"undo", "UNDO"}, {"auto", "AUTO"}, {"padsmode", "PADS"},
    {"keylock", "KEY LOCK"}, {"rhythm", "RHYTHM"}, {"rh1", "HOLD"}, {"rh2", "STABS"}, {"rh3", "CHOP"},
    {"voicing", "VOICING"}, {"vo1", "CLOSE"}, {"vo2", "OPEN"}, {"vo3", "9/11"},
    {"padset", "PAD SET"}, {"ps1", "SONG"}, {"ps2", "DEGREE"}, {"ps3", "TENSION"}, {"pattern", "PATTERN"},
    {"tone", "TONE"}, {"osc", "OSC"}, {"filter", "FILTER"}, {"env", "ENVELOPE"},
    {"cat1", "STRINGS"}, {"cat2", "CHOIR"}, {"cat3", "SYNTH"}, {"cat4", "ORGAN"}, {"cat5", "E-PIANO"},
    {"cat6", "VINYL"}, {"hint", "Tap a preset to load · double tap = big knob"},
    {"print", "PRINT"}, {"printtitle", "· PRINT"}, {"after", "After PRINT"}, {"autoinfo", "AUTO: the loop plays"},
    {"padsinfo", "PADS: the MPC pads play chords"}, {"r1", "LINK"}, {"r2", "Key"}, {"r3", "Print folder"},
    {"r4", "Version"}};
static const int kNumCaps = (int)(sizeof(kCaptions) / sizeof(kCaptions[0]));

enum
{
    P_TONE = 0, P_ENS, P_DETUNE, P_AGE, P_CUTOFF, P_RESO, P_ENV, P_DRIVE, P_ATTACK, P_DECAY, P_SUSTAIN, P_RELEASE,
    P_DENSITY, P_SPREAD, P_STRUM, P_OCT, P_LEVEL,
    P_CAT, P_AUTO, P_KEYLOCK, P_RHYTHM, P_VOICING, P_PADSET,
    P_NEW, P_UNDO, P_PRINT, P_PRESET0,
    P_HDRLINK = P_PRESET0 + kPresetRows, P_HDRPRESET, P_STATUS, P_PRINTINFO, P_PRINTSTATUS, P_PRINTDIRTXT,
    P_LINKINFO, P_KEYINFO, P_VERSION,
    P_CHORD0,
    P_PRESETTXT0 = P_CHORD0 + genralink::kBars,
    P_PRINTROW0 = P_PRESETTXT0 + kPresetRows,
    P_PADNO0 = P_PRINTROW0 + kPrintRows,
    P_PADNAME0 = P_PADNO0 + kPads,
    P_CAP0 = P_PADNAME0 + kPads,
    P_COL0 = P_CAP0 + kNumCaps,
    P_PRESETLIT0 = P_COL0 + kSteps,
    P_CELL0 = P_PRESETLIT0 + kPresetRows,
    P_FLASH0 = P_CELL0 + kPads, // NEW, UNDO, PRINT
    P_COUNT = P_FLASH0 + 3
};

// a chord hit of the loop
struct Hit { int8_t note[kMaxNotes]; int8_t n = 0; float len = 1.f; float vel = 0.75f; bool start = false; };
struct Chords { Hit h[kSteps]; float swing = 0.5f; bool valid = false; };
// the chords of the 16 pads
struct PadChords { int8_t note[kPads][kMaxNotes]; int8_t n[kPads]; };

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

struct Voice
{
    int note = -1, key = -1;        // key: the MPC note (pads mode) that holds it, -1: the loop
    int stage = 0;                  // 0 release, 1 attack, 2 decay/sustain
    float env = 0.f, vel = 0.f, hz = 440.f, menv = 0.f;
    double gate = 0.0;              // >0 samples left (loop), -1 held (keys)
    int delay = 0;                  // strum: samples before it starts
    float ph[4] = {0.f, 0.f, 0.f, 0.f};
    mpc::dsp::PolyBlepOsc osc[3];
    mpc::dsp::SVF lp1, lp2, f1, f2;
    long age = 0;
};

} // namespace

class GenraPads : public Plugin
{
public:
    GenraPads() : Plugin(makeInfo()), screen_(*this)
    {
        std::vector<std::string> engines(kEngineNames, kEngineNames + NUM_ENGINES);
        addParam(choiceParam("tone", "TONE", engines, E_STRINGS));
        addParam(floatParam("ensemble", "ENSEMBLE", 0.f, 100.f, 60.f, "%", 0));
        addParam(floatParam("detune", "DETUNE", 0.f, 50.f, 12.f, "ct", 0));
        addParam(floatParam("age", "AGE", 0.f, 100.f, 20.f, "%", 0));
        addParam(floatParam("cutoff", "CUTOFF", 0.f, 100.f, 45.f, "", 0));
        addParam(floatParam("reso", "RESO", 0.f, 100.f, 5.f, "%", 0));
        addParam(floatParam("env", "ENV", 0.f, 100.f, 20.f, "%", 0));
        addParam(floatParam("drive", "DRIVE", 0.f, 100.f, 10.f, "%", 0));
        addParam(floatParam("attack", "ATTACK", 0.f, 100.f, 35.f, "", 0));
        addParam(floatParam("decay", "DECAY", 0.f, 100.f, 60.f, "", 0));
        addParam(floatParam("sustain", "SUSTAIN", 0.f, 100.f, 85.f, "%", 0));
        addParam(floatParam("release", "RELEASE", 0.f, 100.f, 50.f, "", 0));
        addParam(floatParam("density", "DENSITY", 0.f, 100.f, 100.f, "%", 0));
        addParam(floatParam("spread", "SPREAD", 0.f, 100.f, 30.f, "%", 0));
        addParam(floatParam("strum", "STRUM", 0.f, 100.f, 10.f, "", 0));
        addParam(intParam("oct", "OCTAVE", -2, 2, 0));
        addParam(floatParam("level", "LEVEL", -40.f, 6.f, -6.f, "dB", 1));
        addParam(choiceParam("cat", "KIND", {kCatNames[0], kCatNames[1], kCatNames[2], kCatNames[3], kCatNames[4], kCatNames[5]}, 0));
        addParam(choiceParam("auto", "AUTO PADS", {"Auto", "Pads"}, 0));
        addParam(boolParam("keylock", "KEY LOCK", true));
        addParam(choiceParam("rhythm", "RHYTHM", {"Hold", "Stabs", "Chop"}, 0));
        addParam(choiceParam("voicing", "VOICING", {"Close", "Open", "9-11"}, 1));
        addParam(choiceParam("padset", "PAD SET", {"Song", "Degree", "Tension"}, 0));
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
        for (int i = 0; i < kPads; ++i) addText(("PADNUMMER " + std::to_string(i + 1)).c_str());
        for (int i = 0; i < kPads; ++i) addText(("PADAKKORD " + std::to_string(i + 1)).c_str());
        for (const auto& c : kCaptions) addText((std::string("CAP ") + c[0]).c_str());
        for (int i = 0; i < kSteps; ++i) addText(("LINIE " + std::to_string(i + 1)).c_str());
        for (int i = 0; i < kPresetRows; ++i) addText(("PRESETLICHT " + std::to_string(i + 1)).c_str());
        for (int i = 0; i < kPads; ++i) addText(("PADZELLE " + std::to_string(i + 1)).c_str());
        for (int i = 0; i < 3; ++i) addText(("BLITZ " + std::to_string(i + 1)).c_str());
        if (numParams() != P_COUNT) std::abort();

        screen_.init();
        for (int i = P_HDRLINK; i < P_COL0; ++i) screen_.text(i);
        for (int i = 0; i < kNumCaps; ++i) screen_.text(P_CAP0 + i, kCaptions[i][1]);
        for (int i = 0; i < kSteps; ++i) screen_.strip(P_COL0 + i, kColFrames);
        for (int i = 0; i < kPresetRows; ++i) { screen_.strip(P_PRESETLIT0 + i, 2); screen_.button(P_PRESET0 + i); }
        for (int i = 0; i < kPads; ++i) screen_.strip(P_CELL0 + i, 3);
        screen_.button(P_NEW, P_FLASH0);
        screen_.button(P_UNDO, P_FLASH0 + 1);
        screen_.button(P_PRINT, P_FLASH0 + 2);
        screen_.watch(P_CAT);
        screen_.watch(P_AUTO);
        screen_.setText(P_VERSION, "GENRA PADS 0.1 · LINK v3");
        for (auto& p : padChords_) { std::memset(&p, 0, sizeof(p)); }
        for (auto& h : padHeld_) h.store(false);
        genralink::Link::clearLoop(loop_);
        PadChords first;
        buildPadChords(nullptr, loop_, first); // C minor until GENRA CHOP tells the key
        publishPads(first);
    }

    ~GenraPads() override
    {
        quit_.store(true);
        if (worker_.joinable()) worker_.join();
    }

    static PluginInfo makeInfo()
    {
        PluginInfo i;
        i.name = "GENRA PADS";
        i.vendor = "Genra";
        i.uniqueId = MPC_FOURCC('G', 'n', 'P', 'd');
        i.version = 100;
        i.isInstrument = true;
        i.numInputs = 0;
        i.numOutputs = 2;
        return i;
    }

    void prepare(double sampleRate, int) override
    {
        sr_ = sampleRate;
        for (Voice& v : voices_)
        {
            for (auto& o : v.osc) o.setSampleRate(sampleRate);
            v.lp1.setSampleRate(sampleRate); v.lp2.setSampleRate(sampleRate);
            v.f1.setSampleRate(sampleRate); v.f2.setSampleRate(sampleRate);
        }
        if (!started_)
        {
            started_ = true;
            link_.open();
            worker_ = std::thread([this] { workerMain(); });
        }
    }

    void reset() override
    {
        for (Voice& v : voices_) { v.note = -1; v.key = -1; v.env = 0.f; v.stage = 0; v.gate = 0.0; v.delay = 0; v.lp1.reset(); v.lp2.reset(); }
        gainSmooth_ = -1.f; wasPlaying_ = false; nPending_ = 0;
        for (float& d : dl_) d = 0.f;
    }

    void process(ProcessContext& ctx) override
    {
        genralink::LinkData* L = link_.data();
        if (L) L->lane[genralink::LANE_PADS].beat.fetch_add(1, std::memory_order_relaxed);
        const bool autoMode = getInt(P_AUTO) == 0;
        const int oct = getInt(P_OCT);
        engine_ = clamp(getInt(P_TONE), 0, NUM_ENGINES - 1);
        const int strum = (int)(sr_ * 0.0008 * get(P_STRUM)); // 0..80 ms between voices

        int r = ready_.load(std::memory_order_acquire);
        if (r >= 0) inUse_.store(r, std::memory_order_release);
        const Chords* ch = r >= 0 ? &chords_[r] : nullptr;
        int pr = padReady_.load(std::memory_order_acquire);
        if (pr >= 0) padInUse_.store(pr, std::memory_order_release);
        const PadChords* pc = pr >= 0 ? &padChords_[pr] : nullptr;

        const Transport& tr = ctx.transport;
        const bool playing = tr.valid && tr.playing && tr.ppqValid;
        const double bpm = tr.tempoValid && tr.bpm > 1.0 ? tr.bpm : 120.0;
        const double spStep = sr_ * 60.0 / bpm / 4.0;
        if (playing && autoMode && ch && ch->valid)
        {
            const double q0 = tr.ppqPos * 4.0, q1 = q0 + ctx.numFrames / spStep;
            if (!wasPlaying_ || nextStep_ < q0 - 1.0 || nextStep_ > q1 + 1.0) nextStep_ = std::ceil(q0 - 1e-6);
            const float swing = clampf(ch->swing, 0.5f, 0.75f);
            while (nextStep_ < q1)
            {
                long n = (long)std::llround(nextStep_);
                int s = (int)(((n % kSteps) + kSteps) % kSteps);
                const Hit& h = ch->h[s];
                if (h.n > 0 && nPending_ < 8)
                {
                    double at = (nextStep_ - q0) * spStep;
                    if (s & 1) at += (swing - 0.5f) * 2.f * spStep;
                    Pending& p = pending_[nPending_++];
                    p.at = at;
                    p.hit = h;
                    p.gate = std::max(0.3, (double)h.len) * spStep;
                }
                curStep_.store(s, std::memory_order_relaxed);
                nextStep_ += 1.0;
            }
            wasPlaying_ = true;
        }
        else
        {
            if (wasPlaying_) for (Voice& v : voices_) if (v.key < 0 && v.gate > 0.0) v.gate = 0.0;
            wasPlaying_ = false;
            nPending_ = 0;
            curStep_.store(playing ? curStepOf(tr) : -1, std::memory_order_relaxed);
        }

        const float cutoff = get(P_CUTOFF) / 100.f;
        const float comp = std::pow(10.f, 15.f * std::pow(1.f - cutoff, 1.3f) / 20.f); // two filters: lift more
        float target = std::pow(10.f, get(P_LEVEL) / 20.f) * comp;
        if (gainSmooth_ < 0.f) gainSmooth_ = target;
        const float envAmt = get(P_ENV) / 100.f;
        const float reso = 0.02f + 0.55f * (get(P_RESO) / 100.f) * (1.f - 0.5f * envAmt);
        const float drive = 1.f + 3.f * get(P_DRIVE) / 100.f;
        const float driveNorm = std::tanh(drive);
        const float ens = get(P_ENS) / 100.f, age = get(P_AGE) / 100.f;
        const float cents = std::pow(2.f, get(P_DETUNE) / 1200.f);
        const float att = 0.002f + 3.f * std::pow(get(P_ATTACK) / 100.f, 2.f);
        const float dec = 0.05f + 3.f * std::pow(get(P_DECAY) / 100.f, 2.f);
        const float sus = get(P_SUSTAIN) / 100.f;
        const float rel = 0.05f + 4.f * std::pow(get(P_RELEASE) / 100.f, 2.f);
        const float attC = 1.f / (float)(sr_ * att);
        const float decC = std::exp(-1.f / (float)(sr_ * dec));
        const float relC = std::exp(-1.f / (float)(sr_ * rel));
        const float mdec = std::exp(-1.f / (float)(sr_ * (0.2 + 0.8 * dec)));
        const float dullC = 1.f - std::exp(-2.f * (float)M_PI * (16000.f - 11500.f * age) / (float)sr_);
        const float crush = age > 0.6f ? std::pow(2.f, 15.f - (age - 0.6f) * 22.f) : 0.f;
        const float isr = 1.f / (float)sr_;
        const float cutBase = 40.f * std::pow(2.f, cutoff * 8.5f);
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
                    int pad = padMap_.padOf(e.note());
                    if (pad < 0) pad = ((e.note() - 36) % kPads + kPads) % kPads;
                    if (pc && pc->n[pad] > 0)
                    {
                        Hit h;
                        h.n = pc->n[pad];
                        for (int k = 0; k < h.n; ++k) h.note[k] = pc->note[pad][k];
                        h.vel = e.velocity() / 127.f;
                        playHit(h, -1.0, e.note(), oct, strum);
                        lastPad_.store(pad, std::memory_order_relaxed);
                        padHeld_[pad] = true;
                    }
                }
                else if (e.isNoteOff())
                {
                    for (Voice& v : voices_) if (v.key == e.note()) { v.gate = 0.0; v.key = -2; }
                    int pad = padMap_.padOf(e.note());
                    if (pad < 0) pad = ((e.note() - 36) % kPads + kPads) % kPads;
                    padHeld_[pad] = false;
                }
                else if (e.isAllNotesOff()) for (Voice& v : voices_) v.gate = 0.0;
            }
            for (int k = 0; k < nPending_;)
            {
                if (pending_[k].at <= (double)i)
                {
                    const Pending& p = pending_[k];
                    playHit(p.hit, p.gate, -1, oct, strum);
                    pending_[k] = pending_[--nPending_];
                }
                else ++k;
            }
            gainSmooth_ += (target - gainSmooth_) * 0.002f;

            // tape: wow and flutter, the same for every voice
            lfoW_ += 0.5f * isr; if (lfoW_ >= 1.f) lfoW_ -= 1.f;
            lfoF_ += 6.8f * isr; if (lfoF_ >= 1.f) lfoF_ -= 1.f;
            const float wow = std::pow(2.f, age * (12.f * sn(lfoW_) + 3.f * sn(lfoF_)) / 1200.f);
            const bool ctrl = (ctrl_++ & 15) == 0;

            float mix = 0.f;
            for (Voice& v : voices_)
            {
                if (v.note < 0) continue;
                if (v.delay > 0) { --v.delay; continue; }
                if (v.gate > 0.0) { v.gate -= 1.0; if (v.gate < 0.0) v.gate = 0.0; }
                const bool held = v.gate < 0.0 || v.gate > 0.0;
                if (!held && v.stage != 0) v.stage = 0;
                if (v.stage == 1) { v.env += attC; if (v.env >= 1.f) { v.env = 1.f; v.stage = 2; } }
                else if (v.stage == 2) v.env = sus + (v.env - sus) * decC;
                else { v.env *= relC; if (v.env < 0.0002f) { v.note = -1; continue; } }
                v.menv *= mdec;
                const float f = v.hz * wow;
                if (ctrl)
                {
                    const float spread = engine_ == E_SUPERSAW ? 2.2f : 1.f;
                    const float c = std::pow(cents, spread);
                    for (int k = 0; k < 3; ++k)
                    {
                        v.osc[k].setWave(engine_ == E_SOFT ? mpc::dsp::Wave::Triangle : mpc::dsp::Wave::Saw);
                        v.osc[k].setFrequency(k == 0 ? f : (k == 1 ? f * c : f / c));
                    }
                    float track = std::sqrt(std::max(30.f, f) / 261.6f);
                    float cut = std::fmin(cutBase * track * (1.f + 2.5f * envAmt * v.env), 15000.f);
                    v.lp1.set(cut, 0.05f);
                    v.lp2.set(cut, reso);
                }
                const float inc = f * isr;
                float raw = 0.f;
                switch (engine_)
                {
                    case E_STRINGS:
                    case E_SUPERSAW:
                        raw = 0.4f * v.osc[0].process() + 0.35f * v.osc[1].process() + 0.35f * v.osc[2].process();
                        if (engine_ == E_SUPERSAW) { v.ph[0] += inc * 0.5f; raw += 0.3f * sn(v.ph[0]); }
                        break;
                    case E_CHOIR_A:
                    case E_CHOIR_O:
                    {
                        float src = 0.5f * v.osc[0].process() + 0.5f * v.osc[1].process();
                        float a = v.f1.process(src, mpc::dsp::SVF::Mode::Bandpass);
                        float b = v.f2.process(src, mpc::dsp::SVF::Mode::Bandpass);
                        raw = 1.1f * a + 0.7f * b + 0.15f * src;
                        break;
                    }
                    case E_SOFT:
                        v.ph[0] += inc * 0.5f;
                        raw = 0.5f * v.osc[0].process() + 0.4f * v.osc[1].process() + 0.4f * sn(v.ph[0]);
                        break;
                    case E_ORGAN:
                        v.ph[0] += inc; v.ph[1] += inc * 2.f; v.ph[2] += inc * 3.f; v.ph[3] += inc * 4.f;
                        raw = 0.5f * sn(v.ph[0]) + 0.32f * sn(v.ph[1]) + 0.24f * sn(v.ph[2]) + 0.16f * sn(v.ph[3]);
                        raw += 0.15f * v.menv * v.menv * sn(v.ph[3] * 2.f); // the key click
                        break;
                    case E_EPIANO:
                    {
                        v.ph[0] += inc; v.ph[1] += inc; v.ph[2] += inc * cents;
                        float idx = (0.2f + 1.6f * v.vel) * v.menv;
                        raw = 0.6f * sn(v.ph[0] + idx * sn(v.ph[1]) * 0.16f) + 0.4f * sn(v.ph[2]);
                        break;
                    }
                    default: break;
                }
                if (v.ph[0] > 64.f) for (float& p : v.ph) p -= std::floor(p);
                float y = v.lp1.process(raw, mpc::dsp::SVF::Mode::Lowpass);
                y = v.lp2.process(y, mpc::dsp::SVF::Mode::Lowpass);
                y = std::tanh(y * drive) / driveNorm;
                mix += y * v.env * (0.4f + 0.6f * v.vel);
            }
            mix *= 0.42f * gainSmooth_;

            // ENSEMBLE: three taps around 12 ms, slowly moving; it makes the stereo
            dl_[dw_] = mix;
            lfo1_ += 0.61f * isr; if (lfo1_ >= 1.f) lfo1_ -= 1.f;
            lfo2_ += 0.83f * isr; if (lfo2_ >= 1.f) lfo2_ -= 1.f;
            lfo3_ += 1.13f * isr; if (lfo3_ >= 1.f) lfo3_ -= 1.f;
            const float base = 0.012f * (float)sr_, depth = (0.001f + 0.004f * ens) * (float)sr_;
            float t1 = tap(base + depth * sn(lfo1_)), t2 = tap(base + depth * sn(lfo2_ + 0.33f)), t3 = tap(base + depth * sn(lfo3_ + 0.66f));
            dw_ = (dw_ + 1) & (kDelay - 1);
            float l = mix * (1.f - 0.5f * ens) + ens * (0.7f * t1 + 0.35f * t2);
            float rr = mix * (1.f - 0.5f * ens) + ens * (0.7f * t3 + 0.35f * t2);

            // AGE: hiss, crackle, dull top, fewer bits
            if (age > 0.f)
            {
                float hiss = age * 0.003f * noise();
                float crackle = 0.f;
                if (noise01() < age * age * 0.0006f) crackle = (noise() > 0.f ? 1.f : -1.f) * age * 0.08f;
                l += hiss + crackle; rr += hiss + crackle;
            }
            dullL_ += dullC * (l - dullL_); dullR_ += dullC * (rr - dullR_);
            l = dullL_; rr = dullR_;
            if (crush > 0.f) { l = std::round(l * crush) / crush; rr = std::round(rr * crush) / crush; }
            l = std::tanh(l * 1.1f) / 1.1f;
            rr = std::tanh(rr * 1.1f) / 1.1f;
            outL[i] = outR ? l : 0.5f * (l + rr);
            if (outR) outR[i] = rr;
        }
        for (int k = 0; k < nPending_; ++k) pending_[k].at -= ctx.numFrames;
        inUse_.store(-1, std::memory_order_release);
        padInUse_.store(-1, std::memory_order_release);
    }

    void saveExtraState(std::vector<uint8_t>& out) override
    {
        std::string s = "GP1\n" + std::to_string(presetRow_.load()) + "\n";
        out.assign(s.begin(), s.end());
    }
    void loadExtraState(const uint8_t* data, size_t size) override
    {
        std::string s(reinterpret_cast<const char*>(data), size);
        if (s.compare(0, 4, "GP1\n") == 0) presetRow_.store(std::atoi(s.c_str() + 4));
        resyncReq_.store(true);
    }

    bool formatValue(int index, float plain, std::string& out) override
    {
        char b[32];
        switch (index)
        {
            case P_CUTOFF:
            {
                float hz = 40.f * std::pow(2.f, plain / 100.f * 8.5f);
                if (hz >= 1000.f) std::snprintf(b, sizeof b, "%.1fk", hz / 1000.f);
                else std::snprintf(b, sizeof b, "%.0f", hz);
                out = b;
                return true;
            }
            case P_ATTACK: msText(b, sizeof b, 0.002f + 3.f * std::pow(plain / 100.f, 2.f)); out = b; return true;
            case P_DECAY: msText(b, sizeof b, 0.05f + 3.f * std::pow(plain / 100.f, 2.f)); out = b; return true;
            case P_RELEASE: msText(b, sizeof b, 0.05f + 4.f * std::pow(plain / 100.f, 2.f)); out = b; return true;
            case P_STRUM: std::snprintf(b, sizeof b, "%.0f ms", plain * 0.8f); out = b; return true;
            case P_OCT: std::snprintf(b, sizeof b, "%+d", (int)std::lround(plain)); out = b; return true;
            case P_DETUNE: std::snprintf(b, sizeof b, "%.0f ct", plain); out = b; return true;
            case P_LEVEL: std::snprintf(b, sizeof b, "%+.1f dB", plain); out = b; return true;
            case P_SPREAD:
                out = plain < 33.f ? "Narrow" : (plain < 66.f ? "Wide" : "Widest");
                return true;
            case P_ENS: case P_AGE: case P_RESO: case P_ENV: case P_DRIVE: case P_SUSTAIN: case P_DENSITY:
                std::snprintf(b, sizeof b, "%.0f %%", plain); out = b; return true;
            default: break;
        }
        if (index >= P_COL0) return false;
        return screen_.format(index, out);
    }

private:
    static const int kDelay = 4096;
    struct Pending { double at; Hit hit; double gate; };

    static void msText(char* b, size_t n, float s)
    {
        if (s >= 1.f) std::snprintf(b, n, "%.1f s", s);
        else std::snprintf(b, n, "%.0f ms", s * 1000.f);
    }
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
    float noise01() { return 0.5f + 0.5f * noise(); }

    float tap(float delay)
    {
        float pos = (float)dw_ - delay;
        while (pos < 0.f) pos += (float)kDelay;
        int a = (int)pos;
        float fr = pos - (float)a;
        float x0 = dl_[a & (kDelay - 1)], x1 = dl_[(a + 1) & (kDelay - 1)];
        return x0 + fr * (x1 - x0);
    }

    // a chord: one voice per note, the next free one (or the quietest); key: the MPC note holding it
    void playHit(const Hit& h, double gate, int key, int oct, int strum)
    {
        for (int k = 0; k < h.n && k < kMaxNotes; ++k)
        {
            int note = clampNote(h.note[k] + 12 * oct);
            Voice* best = nullptr;
            for (Voice& v : voices_) if (v.note < 0) { best = &v; break; }
            if (!best)
            {
                for (Voice& v : voices_) if (v.stage == 0 && (!best || v.env < best->env)) best = &v;
                if (!best) for (Voice& v : voices_) if (!best || v.age < best->age) best = &v;
            }
            Voice& v = *best;
            v.note = note;
            v.key = key;
            v.hz = (float)(440.0 * std::pow(2.0, (note - 69) / 12.0));
            v.vel = 0.3f + 0.7f * h.vel;
            v.stage = 1;
            if (v.env > 0.3f) v.env = 0.3f; // a stolen voice starts again softly, no click
            v.menv = 1.f;
            v.gate = gate;
            v.delay = k * strum;
            v.age = ++voiceClock_;
            if (engine_ == E_CHOIR_A || engine_ == E_CHOIR_O)
            {
                const bool ah = engine_ == E_CHOIR_A;
                v.f1.set(ah ? 780.f : 420.f, 0.8f);
                v.f2.set(ah ? 1180.f : 780.f, 0.8f);
            }
        }
    }

    // ------------------------------------------------------------------ worker: chords
    static void sortNotes(int* n, int c) { std::sort(n, n + c); }

    // VOICING and SPREAD on a set of notes (any order); root: the bar's root (for 9-11), -1 none
    void voice(int* n, int& c, int root, int voicing, float spread) const
    {
        if (c <= 0) return;
        sortNotes(n, c);
        // close: everything within an octave above the lowest note, around C4
        const int low = n[0];
        for (int k = 1; k < c; ++k) { while (n[k] - low >= 12) n[k] -= 12; while (n[k] < low) n[k] += 12; }
        sortNotes(n, c);
        int dedup = 1;
        for (int k = 1; k < c; ++k) if (n[k] != n[dedup - 1]) n[dedup++] = n[k];
        c = dedup;
        if (voicing == 2 && root >= 0 && root < 12)
        {
            int add[2] = {root + 2, root + 5}; // the 9th and the 11th
            for (int a : add)
            {
                if (c >= kMaxNotes - 1) break;
                int pcA = ((a % 12) + 12) % 12;
                bool have = false;
                for (int k = 0; k < c; ++k) if (((n[k] % 12) + 12) % 12 == pcA) have = true;
                if (have) continue;
                int p = n[c - 1] - ((n[c - 1] - pcA) % 12 + 12) % 12 + 12;
                n[c++] = p;
            }
            sortNotes(n, c);
        }
        int mid = 0;
        for (int k = 0; k < c; ++k) mid += n[k];
        mid /= c;
        int shift = 0;
        while (mid + shift > 66) shift -= 12;
        while (mid + shift < 54) shift += 12;
        for (int k = 0; k < c; ++k) n[k] += shift;
        if (voicing == 1 && c >= 3) { n[c - 2] -= 12; sortNotes(n, c); } // drop 2
        if (spread >= 0.33f && c >= 3) n[0] -= 12;
        if (spread >= 0.66f && c >= 3) n[c - 1] += 12;
        sortNotes(n, c);
    }

    void buildChords(const genralink::Loop& l, Chords& out)
    {
        const int rhythm = getInt(P_RHYTHM), voicing = getInt(P_VOICING);
        const float dens = get(P_DENSITY) / 100.f, spread = get(P_SPREAD) / 100.f;
        out = Chords();
        out.swing = l.swingX1000 / 1000.f;
        out.valid = true;
        // the chord that sounds on each step of the loop
        int cn[kSteps][4], cc[kSteps];
        int cur[4] = {-1, -1, -1, -1}, curC = 0;
        bool startAt[kSteps];
        for (int s = 0; s < kSteps; ++s)
        {
            startAt[s] = false;
            if (l.pad[s][0] >= 0)
            {
                curC = 0;
                for (int v = 0; v < 4; ++v) if (l.pad[s][v] >= 0) cur[curC++] = l.pad[s][v];
                startAt[s] = true;
            }
            cc[s] = curC;
            for (int v = 0; v < 4; ++v) cn[s][v] = cur[v];
        }
        // before the first chord: the last one (the loop goes round)
        int first = 0;
        while (first < kSteps && !startAt[first]) ++first;
        if (first < kSteps)
            for (int s = 0; s < first; ++s) { cc[s] = cc[kSteps - 1]; for (int v = 0; v < 4; ++v) cn[s][v] = cn[kSteps - 1][v]; }
        auto hitAt = [&](int s) -> bool {
            if (rhythm == 0) return startAt[s];
            uint32_t h = (uint32_t)s * 2654435761u ^ l.id * 40503u;
            bool keep = dens >= 0.999f || ((h >> 8) & 255) / 255.f <= dens;
            if (rhythm == 1)
            {
                static const char* kStab = "x.....x...x.....";
                return kStab[s % 16] == 'x' && (s % 16 == 0 || keep);
            }
            bool any = false;
            for (int k = 0; k < kSteps; ++k) if (l.chop[k]) { any = true; break; }
            if (!any) return startAt[s];
            return l.chop[s] != 0 && (startAt[s] || keep);
        };
        for (int s = 0; s < kSteps; ++s)
        {
            if (!hitAt(s) || cc[s] == 0) continue;
            int n[kMaxNotes], c = cc[s];
            for (int v = 0; v < c; ++v) n[v] = cn[s][v];
            voice(n, c, l.barRoot[s / 16], voicing, spread);
            Hit& h = out.h[s];
            h.n = (int8_t)c;
            for (int k = 0; k < c; ++k) h.note[k] = (int8_t)n[k];
            h.start = true;
            h.vel = rhythm == 1 ? 0.8f : 0.7f;
            int next = s + 1;
            while (next < kSteps && !hitAt(next)) ++next;
            if (rhythm == 0) h.len = (float)(next - s) - 0.05f;
            else if (rhythm == 1) h.len = 1.5f;
            else h.len = (float)std::min(4, next - s) - 0.1f;
        }
    }

    void buildPadChords(genralink::LinkData* L, const genralink::Loop& l, PadChords& out)
    {
        std::memset(&out, 0, sizeof(out));
        const int voicing = getInt(P_VOICING), set = getInt(P_PADSET);
        const float spread = get(P_SPREAD) / 100.f;
        int key = L ? L->keyRoot.load() : -1;
        bool minor = L ? L->keyMinor.load() != 0 : true;
        if (key < 0 || key > 11) { key = 0; minor = true; }
        struct C { int root; int ct; bool up = false; };
        C list[kPads];
        const Deg* deg = set == 2 ? (minor ? kTenMinor : kTenMajor) : (minor ? kDegMinor : kDegMajor);
        for (int p = 0; p < kPads; ++p) list[p] = {(key + deg[p].rel) % 12, deg[p].ct, deg[p].rel >= 12};
        if (set == 0) // SONG: the bar chords, with 9ths, the chops' chords, the bar chords an octave up
        {
            int nb = 0;
            C bars[4];
            for (int b = 0; b < 4; ++b)
                if (l.barRoot[b] >= 0 && l.barQuality[b] >= 0 && l.barQuality[b] < 8)
                {
                    C c{l.barRoot[b], kLinkQ[l.barQuality[b]], false};
                    bool dup = false;
                    for (int k = 0; k < nb; ++k) if (bars[k].root == c.root && bars[k].ct == c.ct) dup = true;
                    if (!dup) bars[nb++] = c;
                }
            auto ninth = [](int ct) { return ct == C_MIN || ct == C_M7 ? C_M9 : (ct == C_MAJ || ct == C_MAJ7 ? C_MAJ9 : (ct == C_7 ? C_9 : ct)); };
            for (int k = 0; k < nb; ++k) { list[k] = bars[k]; list[4 + k] = {bars[k].root, ninth(bars[k].ct)}; list[12 + k] = {bars[k].root, bars[k].ct, true}; }
            int nc = 0;
            if (L)
                for (int i = 0; i < genralink::kChops && nc < 4; ++i)
                {
                    int r = L->chordRoot[i].load(), q = L->chordQuality[i].load();
                    if (r < 0 || q < 0 || q > 7) continue;
                    C c{r, kLinkQ[q], false};
                    bool dup = false;
                    for (int k = 0; k < nb; ++k) if (bars[k].root == c.root && bars[k].ct == c.ct) dup = true;
                    for (int k = 0; k < nc; ++k) if (list[8 + k].root == c.root && list[8 + k].ct == c.ct) dup = true;
                    if (!dup) list[8 + nc++] = c;
                }
        }
        for (int p = 0; p < kPads; ++p)
        {
            const ChordType& t = kCT[list[p].ct];
            int root = ((list[p].root % 12) + 12) % 12, octUp = list[p].up ? 12 : 0;
            int n[kMaxNotes + 2], c = 0;
            for (int k = 0; k < t.n && c < kMaxNotes; ++k) n[c++] = 48 + root + t.iv[k];
            voice(n, c, root, voicing, spread);
            out.n[p] = (int8_t)std::min(c, kMaxNotes);
            for (int k = 0; k < out.n[p]; ++k) out.note[p][k] = (int8_t)(n[k] + octUp);
            padName_[p] = std::string(kNotes[root]) + t.suffix + (octUp ? " +" : "");
        }
    }

    void publishChords(const Chords& c)
    {
        int r = ready_.load(), u = inUse_.load(), w = 0;
        while (w == r || w == u) ++w;
        chords_[w] = c;
        ready_.store(w, std::memory_order_release);
        shown_ = c;
    }
    void publishPads(const PadChords& c)
    {
        int r = padReady_.load(), u = padInUse_.load(), w = 0;
        while (w == r || w == u) ++w;
        padChords_[w] = c;
        padReady_.store(w, std::memory_order_release);
        shownPads_ = c;
    }

    // ------------------------------------------------------------------ worker: screen
    void display(bool padsMode)
    {
        const int step = curStep_.load();
        int voicesAt[kSteps] = {};
        bool startAt[kSteps] = {};
        if (!padsMode)
            for (int s = 0; s < kSteps; ++s)
            {
                const Hit& h = shown_.h[s];
                if (h.n <= 0) continue;
                startAt[s] = true;
                int len = std::max(1, (int)std::lround(h.len));
                for (int k = 0; k < len && s + k < kSteps; ++k) voicesAt[s + k] = std::min(4, (int)h.n);
            }
        for (int s = 0; s < kSteps; ++s)
            screen_.setFrame(P_COL0 + s, padsMode ? 0 : voicesAt[s] * 4 + (startAt[s] ? 2 : 0) + (s == step ? 1 : 0));
        const int lit = lastPad_.load();
        for (int p = 0; p < kPads; ++p)
        {
            screen_.setFrame(P_CELL0 + p, padsMode ? (p == lit && litUntil_ > nowMs() ? 2 : 1) : 0);
            screen_.setText(P_PADNO0 + p, padsMode ? "PAD " + std::to_string(p + 1) : std::string());
            screen_.setText(P_PADNAME0 + p, padsMode && shownPads_.n[p] > 0 ? padName_[p] : std::string());
        }
    }

    long long nowMs() const
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    static std::string chordName(int r, int q)
    {
        if (r < 0 || r > 11) return "-";
        return std::string(kNotes[r]) + (q >= 0 && q < 8 ? kCT[kLinkQ[q]].suffix : "");
    }

    std::string keyOf(genralink::LinkData* L) const
    {
        int kr = L ? L->keyRoot.load() : -1;
        return kr >= 0 && kr < 12 ? std::string(kNotes[kr]) + (L->keyMinor.load() ? "m" : "") : std::string("-");
    }

    void texts(genralink::LinkData* L, bool alive, bool padsMode)
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
        const char* sets[3] = {"song", "degrees", "tension"};
        const int setNo = clamp(getInt(P_PADSET), 0, 2);
        if (padsMode)
        {
            screen_.setText(P_CHORD0, "PAD CHORDS · " + key + " · " + sets[setNo]);
            for (int bar = 1; bar < genralink::kBars; ++bar) screen_.setText(P_CHORD0 + bar, std::string());
        }
        else for (int bar = 0; bar < genralink::kBars; ++bar) screen_.setText(P_CHORD0 + bar, chordName(loop_.barRoot[bar], loop_.barQuality[bar]));
        const char* rh[3] = {"chords held", "stabs", "chords on the chops"};
        const int rhythm = clamp(getInt(P_RHYTHM), 0, 2);
        int step = curStep_.load();
        const int lit = lastPad_.load();
        if (padsMode)
        {
            if (lit >= 0 && lit < kPads && shownPads_.n[lit] > 0)
                std::snprintf(b, sizeof b, "Pad %d · %s · %d voices", lit + 1, padName_[lit].c_str(), (int)shownPads_.n[lit]);
            else std::snprintf(b, sizeof b, "PADS · the MPC pads play %s", sets[setNo]);
            screen_.setText(P_STATUS, b);
        }
        else if (step >= 0)
        {
            int bar = step / 16;
            std::snprintf(b, sizeof b, "Bar %d · %s · %s", bar + 1, chordName(loop_.barRoot[bar], loop_.barQuality[bar]).c_str(), rh[rhythm]);
            screen_.setText(P_STATUS, b);
        }
        else screen_.setText(P_STATUS, std::string("AUTO · ") + rh[rhythm]);
        std::snprintf(b, sizeof b, "Pads · 4 bars · %.0f BPM · %s", bpm100 / 100.0, key.c_str());
        screen_.setText(P_PRINTINFO, b);
        screen_.setText(P_PRINTSTATUS, getInt(P_AUTO) == 0 ? "AUTO: the loop plays" : "PADS: the MPC pads play chords");
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
        if (genralink::LinkData* L = link_.data()) genralink::Link::writeName(L->lane[genralink::LANE_PADS], p.name);
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
        const double strumSteps = get(P_STRUM) * 0.0008 * (L && L->bpmX100.load() > 0 ? L->bpmX100.load() / 100.0 : 120.0) / 15.0;
        const float swing = clampf(shown_.swing, 0.5f, 0.75f);
        std::vector<MidiNote> notes;
        for (int s = 0; s < kSteps; ++s)
        {
            const Hit& h = shown_.h[s];
            if (h.n <= 0) continue;
            double at = s + ((s & 1) ? (swing - 0.5f) * 2.f : 0.f);
            for (int k = 0; k < h.n; ++k)
                notes.push_back({at + k * strumSteps, std::max(0.3, (double)h.len), clampNote(h.note[k] + 12 * oct), (int)std::lround(h.vel * 127.f)});
        }
        int bpm100 = L ? L->bpmX100.load() : 0;
        double bpm = bpm100 > 0 ? bpm100 / 100.0 : 120.0;
        int no = printNumberFor(dir, loop_.id);
        char name[96];
        std::snprintf(name, sizeof name, "Print %02d Pads %s %.0f", no, keyOf(L).c_str(), bpm);
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
        int lastRhythm = -1, lastVoicing = -1, lastSet = -1, lastKey = -2;
        float lastDens = -1.f, lastSpread = -1.f;
        bool padsBuilt = false;
        genralink::Loop tmp;
        genralink::Link::Alive alive;
        padMap_.load();
        if (presetRow_.load() < 0) applyPreset(clamp(getInt(P_CAT), 0, kCats - 1), 0); else rememberSound();
        long long lastSlow = 0;
        int seenPad = -1;
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
            int rh = getInt(P_RHYTHM), vo = getInt(P_VOICING), ps = getInt(P_PADSET);
            float de = get(P_DENSITY), sp = get(P_SPREAD);
            int keyCode = L ? L->keyRoot.load() * 2 + (L->keyMinor.load() ? 1 : 0) : -1;
            const bool changed = rh != lastRhythm || vo != lastVoicing || std::fabs(de - lastDens) > 0.5f || std::fabs(sp - lastSpread) > 0.5f;
            if (newLoop || changed)
            {
                Chords c;
                buildChords(loop_, c);
                publishChords(c);
            }
            // the pad chords follow the key (KEY LOCK), the loop's chords and the settings
            if (!padsBuilt || ((newLoop || keyCode != lastKey) && getBool(P_KEYLOCK)) || changed || ps != lastSet)
            {
                PadChords pc;
                buildPadChords(L, loop_, pc);
                publishPads(pc);
                padsBuilt = true;
                lastKey = keyCode;
            }
            lastRhythm = rh; lastVoicing = vo; lastSet = ps; lastDens = de; lastSpread = sp;
            int lp = lastPad_.load();
            if (lp >= 0 && (lp != seenPad || padHeld_[lp])) { litUntil_ = nowMs() + 250; seenPad = lp; }
            for (auto& t : screen_.taps())
            {
                if (t.first == P_NEW && L) { L->lane[genralink::LANE_PADS].newRequest.fetch_add(1); screen_.message(P_STATUS, on ? "NEW: new pads from the next bar" : "no GENRA CHOP"); }
                else if (t.first == P_UNDO && L) { L->lane[genralink::LANE_PADS].undoRequest.fetch_add(1); screen_.message(P_STATUS, on ? "UNDO: the last pads" : "no GENRA CHOP"); }
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
                    genralink::Link::writeName(L->lane[genralink::LANE_PADS], nm);
                }
            }
            const bool padsMode = getInt(P_AUTO) == 1;
            texts(L, on, padsMode);
            display(padsMode);
            screen_.push();
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
        }
    }

    genraui::Screen screen_;
    genralink::Link link_;
    PadMap padMap_;
    bool started_ = false;
    std::thread worker_;
    std::atomic<bool> quit_{false}, resyncReq_{false};
    Chords chords_[3];
    Chords shown_;
    PadChords padChords_[3];
    PadChords shownPads_{};
    std::string padName_[kPads];
    genralink::Loop loop_;
    std::atomic<int> ready_{-1}, inUse_{-1}, padReady_{-1}, padInUse_{-1}, curStep_{-1}, presetRow_{-1}, lastPad_{-1};
    std::atomic<bool> padHeld_[kPads];
    long long litUntil_ = 0;
    int presetCat_ = 0;
    bool edited_ = false;
    float soundSeen_[kSoundN] = {};
    double sr_ = 44100.0;
    Voice voices_[kVoices];
    long voiceClock_ = 0;
    unsigned ctrl_ = 0;
    uint32_t rng_ = 0x13579bdfu;
    int engine_ = E_STRINGS;
    float gainSmooth_ = -1.f, dullL_ = 0.f, dullR_ = 0.f;
    float lfoW_ = 0.f, lfoF_ = 0.f, lfo1_ = 0.f, lfo2_ = 0.f, lfo3_ = 0.f;
    float dl_[kDelay] = {};
    int dw_ = 0;
    double nextStep_ = 0.0;
    bool wasPlaying_ = false;
    Pending pending_[8];
    int nPending_ = 0;
};

MPC_PLUGIN_ENTRY(GenraPads)
