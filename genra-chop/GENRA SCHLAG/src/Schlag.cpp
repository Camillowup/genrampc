// SCHLAG - the drum companion of GENRA CHOP: eight sampled voices that follow GENRA CHOP's drums, with the
// random sample of the Polyend Play: a voice can swap its sample on some of its hits for others from its folder.
//
// SPDX-License-Identifier: GPL-3.0-only
//
// Pages (genra-chop/tools/build_skin.py), on the function keys:
//   SCHLAG  eight columns KICK, SNARE, CLAP, RIM, HAT ZU, HAT OFFEN, TOM, PERC: lamp, M / S, folder and sample
//           (tap: the voice for the ORDNER page), the bar's hits (red: a swapped sample), RANDOM (how many hits
//           swap; every turn rolls again; 0 % = the voice's own sample on every hit), four function knobs (tap the
//           function's name to step through TUNE, DECAY, DRIVE, BITS, REPEAT, PAN, REVERSE, FILTER), the eight pads
//           to play; header: LINK, AUTO | PADS, UNDO, BASIC (every RANDOM to 0), NEW. CLAP and TOM have displays that
//           are dying (on purpose).
//   ORDNER  the voice (8 buttons), its folder: rows to tap (folder: open; WAV: first tap = listen, second tap = the
//           voice's sample; its folder becomes the voice's folder), ZURUECK, page buttons, HAUPTORDNER HIER, LISTE
//   PRINT   the drums as a MIDI file (pad notes); afterwards PADS
//   SETUP   LERNEN (the pads, shared with every GENRA plugin), CHOKE (closed hat cuts the open hat), the status
//
// The pattern comes from GENRA CHOP over LINK v3 (common/genra_link.h): velocity, timing, ratchets, chance, every-2nd
// per voice and step. AUTO plays it on the MPC's transport; PADS only the notes of this track (pads 1-8 = voices).
// NEW / UNDO ask GENRA CHOP for new drums / the last ones.
//
// Signal flow per hit: sample (16-bit in memory, up to 24 per folder, 2 s each) -> TUNE (rate) / REVERSE -> DECAY
// -> BITS -> DRIVE -> FILTER -> PAN -> LEVEL, velocity -> sum -> VOLUME -> a fixed peak limiter -> out 1/2.
// Hall and delay: the MPC track's sends. Threads: process() (audio) only plays and touches atomics; a worker loads
// folders, builds the screen and writes files.

#include "mpc_plugin.h"
#include "../../common/genra_files.h"
#include "../../common/genra_link.h"
#include "../../common/genra_ui.h"
#include "../../common/genra_diag.h"

#include <algorithm>
#include <limits>
#include <chrono>
#include <cmath>
#include <memory>
#include <mutex>
#include <thread>

using namespace mpc;
using namespace genrafiles;

namespace {

static const int kVoices = 8, kSlots = 2, kSteps = genralink::kSteps, kRows = 8, kPrintRows = 7;
static const int kSegs = 3, kChips = 4, kLvlFrames = 21; // 0.15
static const int kMaxFiles = 128, kFlashes = 31; // 0.15: + COPY BAR, PASTE, CLEAR BAR; 0.16: + DIAG, SKIN
static const size_t kMaxVoiceBytes = 16u << 20; // the samples of one voice: at most 16 MB in memory
// MIDI channel 10 (GENRA CHOP OUT): the notes of pads A01-A08 of a new MPC drum program, voice order KICK .. PERC
const int kKitNote[8] = {37, 36, 42, 82, 40, 38, 46, 44};
static const float kMaxSeconds = 2.f;
const char* const kVoiceNames[kVoices] = {"KICK", "SNARE", "CLAP", "RIM", "CL HAT", "OP HAT", "TOM", "PERC"};
enum Func { F_TUNE, F_DECAY, F_DRIVE, F_BITS, F_REPEAT, F_PAN, F_REVERSE, F_FILTER, NUM_FUNCS };
const char* const kFuncNames[NUM_FUNCS] = {"TUNE", "DECAY", "DRIVE", "BITS", "REPEAT", "PAN", "REVERSE", "FILTER"};
const int kDefaultFunc[kSlots] = {F_TUNE, F_DECAY};
// what RANDOM does to the hits it picks (per voice)
enum RandomKind { RK_SAMPLE, RK_POOL, RK_PITCH, RK_GHOST, RK_SHORT, RK_FLIP, RK_TEXTURE, NUM_RKINDS };
const char* const kRandomKinds[NUM_RKINDS] = {"SAMPLE", "POOL", "PITCH", "GHOST", "SHORT", "FLIP", "TEXTURE"};
// STEPS: per step RANDOM and the 8 functions (a lock overrides the main page for that step only)
// lock order: 0 RANDOM, 1..8 the functions, 9 LEVEL (dB), 10 CHANCE (%)
// Q-LINKS: FUNCTIONS (0.12): the chosen voice's column from top to bottom: SAMPLE, RANDOM, the upper and the lower
// function slot, then LEVEL
enum QLink { QL_SAMPLE, QL_RANDOM, QL_UPPER, QL_LOWER, QL_LEVEL, QL_KIND, NUM_QL }; // Q1-5, Q6 = KIND
static const int kQL = NUM_QL;
const char* const kQLNames[NUM_QL] = {"SAMPLE", "RANDOM", "UPPER", "LOWER", "LEVEL", "KIND"};
// STEPS (0.12): two knobs, each with a field whose < > choose what it sets for the step
enum StepFn { SF_SAMPLE, SF_COND, SF_LEVEL, SF_CHANCE, SF_RANDOM, SF_TUNE, SF_DECAY, SF_DRIVE, SF_BITS, SF_REPEAT, SF_PAN,
              SF_REVERSE, SF_FILTER, NUM_SF };
const char* const kStepFnNames[NUM_SF] = {"SAMPLE", "CONDITION", "LEVEL", "CHANCE", "RANDOM", "TUNE", "DECAY", "DRIVE",
                                          "BITS", "REPEAT", "PAN", "REVERSE", "FILTER"};
static const int kStepKnobs = 2;
const int kStepFnDefault[kStepKnobs] = {SF_SAMPLE, SF_TUNE};
static const int kLocks = 3 + NUM_FUNCS, kGrid = 8 * 16, LK_LEVEL = 1 + NUM_FUNCS, LK_CHANCE = 2 + NUM_FUNCS;
// CONDITION per step (pass of the loop): 0 = none (GENRA's), then n of 2 / n of 4 / not the 1st pass
static const int NUM_CONDS = 8;
const char* const kCondNames[NUM_CONDS] = {"—", "1st of 2", "2nd of 2", "1st of 4", "2nd of 4", "3rd of 4", "4th of 4", "not 1st"};

static const char* const kCaptions[][2] = {
    {"schlag", "SCHLAG"}, {"auto", "GENRA"}, {"padsmode", "TRACK"}, {"undo", "UNDO"}, {"basic", "BASIC"}, {"new", "NEW"},
    {"m", "M"}, {"s", "S"}, {"v1", "KICK"}, {"v2", "SNARE"}, {"v3", "CLAP"}, {"v4", "RIM"}, {"v5", "CL HAT"},
    {"v6", "OP HAT"}, {"v7", "TOM"}, {"v8", "PERC"}, {"p1", "1"}, {"p2", "2"}, {"p3", "3"}, {"p4", "4"},
    {"p5", "5"}, {"p6", "6"}, {"p7", "7"}, {"p8", "8"}, {"ordner", "· FOLDER"}, {"back", "BACK"},
    {"drives", "DRIVES"}, {"pgup", "< PAGE"}, {"pgdn", "PAGE >"}, {"root", "ROOT HERE"},
    {"hint1", "1st tap = listen"}, {"hint2", "2nd tap = the voice's sample"}, {"print", "PRINT"},
    {"printtitle", "· PRINT"}, {"after", "After PRINT"}, {"autoinfo", "GENRA: GENRA's loop plays"},
    {"padsinfo", "TRACK: only the track's notes"}, {"learn", "LEARN"}, {"help1", "Tap LEARN, then press"},
    {"help2", "pads 1 to 16 in order"}, {"help3", "(pad 1 = bottom left)"},
    {"help4", "Used by every GENRA plugin"}, {"choke", "HAT CHOKE"}, {"r1", "Pad map"}, {"r2", "Root folder"},
    {"r3", "LINK"}, {"r4", "Memory"}, {"r5", "Version"}, {"diag", "DIAG"}, {"skin", "SKIN"}, {"setup", "· SETUP"}, {"savesmp", "SAVE SAMPLE"},
    {"savekit", "SAVE KIT"}, {"kitnotes", "MPC KIT NOTES"}, {"kithelp1", "MIDI ch 10: MPC kit notes"},
    {"kithelp2", "(A01-A08 = KICK .. PERC)"}, {"kithelp3", "ON: on every channel"}, {"loadsmp", "LOAD SAMPLE"}, {"stepstitle", "· STEPS"},
    {"bar1", "BAR 1"}, {"bar2", "BAR 2"}, {"bar3", "BAR 3"}, {"bar4", "BAR 4"}, {"clearstep", "CLEAR STEP"},
    {"lk0", "RANDOM"}, {"lk1", "TUNE"}, {"lk2", "DECAY"}, {"lk3", "DRIVE"}, {"lk4", "BITS"}, {"lk5", "REPEAT"},
    {"lk6", "PAN"}, {"lk7", "REVERSE"}, {"lk8", "FILTER"}, {"qlfuncs", "Q-LINKS: FUNCTIONS"},
    {"steph1", "tap a step to set its own values"}, {"steph2", "— = the main page's value"}, {"lk9", "LEVEL"},
    {"lk10", "CHANCE"}, {"smpcap", "SAMPLE"}, {"condcap", "CONDITION"}, {"hit1", "HIT"}, {"hit2", "ON / OFF"},
    {"lt", "<"}, {"gt", ">"}, {"qlall", "Q-LINKS: ALL SLOTS 1-8 / 9-16"}, {"clearall", "CLEAR ALL"}, {"empty", "EMPTY"}, {"loadkit", "LOAD KIT"},
    // 0.15: the new pages
    {"twice", "tap twice"}, {"preview", "PREVIEW"}, {"roll", "ROLL"}, {"lvl", "LEVEL"}, {"ql0", "Q: RANDOM / LEVEL"},
    {"ql0s", "press SCHLAG: voice"}, {"ql1s", "press SCHLAG: all slots"}, {"ql2", "Q: ALL SLOTS"},
    {"ql2s", "press SCHLAG: RANDOM"}, {"draw", "DRAW"}, {"edit", "EDIT"}, {"drawsub", "tap = hit on / off"},
    {"editsub", "tap = choose"}, {"follow", "FOLLOW"}, {"followsub", "page = play head"}, {"copybar", "COPY BAR"},
    {"paste", "PASTE"}, {"clearbar", "CLEAR BAR"}, {"own", "own values:"}, {"chiphelp", "tap a chip = on the knob"},
    {"listhelp", "2x tap = list"}, {"kit", "KIT"}, {"savekitsub", "all 8 voices"}, {"loadkitsub", "opens the Kits folder"},
    {"savesmpsub", "with its functions"}, {"folderup", "FOLDER UP"}, {"list", "LIST"}, {"listq", "Q-Link 13"}, {"gt2", "›"}, {"tapgo", "tap = go there"}};
static const int kNumCaps = (int)(sizeof(kCaptions) / sizeof(kCaptions[0]));

enum
{
    // per voice v: RANDOM (Q-Links 1-8), LEVEL (Q-Links 9-16), then the rest
    P_RANDOM0 = 0,
    P_LEVEL0 = P_RANDOM0 + kVoices,
    P_MUTE0 = P_LEVEL0 + kVoices,
    P_SOLO0 = P_MUTE0 + kVoices,
    P_FUNC0 = P_SOLO0 + kVoices,          // v * kSlots + k: the function a slot shows
    P_VAL0 = P_FUNC0 + kVoices * kSlots,  // v * kSlots + k: the slot's knob (the value of the function it shows)
    P_FV0 = P_VAL0 + kVoices * kSlots,    // v * NUM_FUNCS + f: every function's value (works shown or not)
    P_VOLUME = P_FV0 + kVoices * NUM_FUNCS, P_AUTO, P_CHOKE, P_VOICE, P_KITNOTES,
    P_LIVE0,                              // per voice: RANDOM HOLD (0) or LIVE (1)
    P_RKIND0 = P_LIVE0 + kVoices,         // per voice: what RANDOM does (SAMPLE .. TEXTURE)
    P_BAR = P_RKIND0 + kVoices,           // STEPS: the bar shown
    P_SLK0,                               // STEPS: the selected step's RANDOM + 8 functions (knobs)
    P_QL0 = P_SLK0 + kLocks,              // Q-LINKS: FUNCTIONS: stand-ins for the chosen voice (tap its name)
    P_SKF0 = P_QL0 + kQL,                 // STEPS: what the two knobs set (SAMPLE .. FILTER)
    P_SKN0 = P_SKF0 + kStepKnobs,         // STEPS: the two knobs (0..100, stand-ins)
    P_PREVIEW = P_SKN0 + kStepKnobs,      // 0.15: PREVIEW: a sample chosen (< >, Q-Link, list, STEPS) plays once
    P_DRAW,                               // 0.15: STEPS: DRAW (tap = hit on / off) or EDIT (tap = choose)
    P_FOLLOW,                             // 0.15: STEPS: the page follows the play head
    // buttons
    P_NEW, P_UNDO, P_BASIC, P_PRINT, P_LEARN, P_BACK, P_DRVBTN, P_PGUP, P_PGDN, P_ROOTHERE, P_SAVESMP, P_SAVEKIT, P_LOADSMP, P_CLEARSTEP,
    P_SMPPREV, P_SMPNEXT, P_CONDPREV, P_CONDNEXT, P_HIT, P_CLEARALL, P_EMPTY, P_LOADKIT,
    P_SKFTAP0,                            // STEPS: knob k's field: < = P_SKFTAP0 + 2k, > = + 2k + 1 (flashes 0..25)
    P_COPYBAR = P_SKFTAP0 + 2 * kStepKnobs, P_PASTE, P_CLEARBAR, // 0.15 (flashes 26..28)
    P_DIAG, P_SKIN,                       // 0.16: SETUP (flashes 29, 30)
    P_SCROLL,
    P_FUNCTAP0,                            // v * kSlots + k: the function's name, tap = next function
    P_PICK0 = P_FUNCTAP0 + kVoices * kSlots, // folder / sample fields: the voice for ORDNER
    P_PADTAP0 = P_PICK0 + kVoices,
    P_ROWTAP0 = P_PADTAP0 + kVoices,
    P_RKINDTAP0 = P_ROWTAP0 + kRows,      // tap the RANDOM field: the next kind
    P_STEPTAP0 = P_RKINDTAP0 + kVoices,   // STEPS: v * 16 + column
    P_LANETAP0 = P_STEPTAP0 + kGrid,      // STEPS: a voice's name = the whole lane in the editor
    P_VPREV0 = P_LANETAP0 + kVoices,      // main page: < per voice (the previous sample of its folder)
    P_VNEXT0 = P_VPREV0 + kVoices,        // main page: > per voice
    P_ROLL0 = P_VNEXT0 + kVoices,         // 0.15: ROLL per voice: RANDOM rolls again (HOLD keeps the new roll)
    P_SEGTAP0 = P_ROLL0 + kVoices,        // 0.15: FOLDER: the path's parts (tap = go there)
    P_CHIPTAP0 = P_SEGTAP0 + kSegs,       // 0.15: STEPS: the step's own values as chips (tap = on knob 1)
    // texts
    P_HDRLINK = P_CHIPTAP0 + kChips, P_PATH, P_LISTINFO, P_VOICEINFO, P_VFOLDER, P_VSAMPLE, P_VCOUNT, P_STATUS,
    P_PRINTINFO, P_PRINTSTATUS, P_PRINTDIRTXT, P_LEARNINFO, P_MAPINFO, P_ROOTTXT, P_LINKINFO, P_MEMINFO, P_VERSION, P_STEPHDR, P_STEPPOS, P_SMPTXT, P_CONDTXT, P_QLTXT,
    P_KITTXT, P_PAGETXT,                  // 0.15: the kit's name, the list's page
    P_DIAGINFO,                           // 0.16: SETUP: DIAG's state
    P_SKFTXT0,                            // STEPS: the knobs' fields (function names)
    P_SKVTXT0 = P_SKFTXT0 + kStepKnobs,   // STEPS: the knobs' values
    P_SLKTXT0_ = P_SKVTXT0 + kStepKnobs,
    P_SLKTXT0 = P_SLKTXT0_,               // the step knobs' values ("—" = the main page's)
    P_SEGTXT0 = P_SLKTXT0 + kLocks,       // 0.15: FOLDER: the path's parts
    P_CHIPTXT0 = P_SEGTXT0 + kSegs,       // 0.15: STEPS: the chips
    P_BARDOT0 = P_CHIPTXT0 + kChips,      // 0.15: STEPS: dots under BAR 1-4 (the bar has hits)
    P_MORETXT0 = P_BARDOT0 + 4,           // 0.15: main page: "+2" = functions off neutral that no slot shows
    P_RINGTXT0 = P_MORETXT0 + kVoices,    // the RANDOM amount in the ring
    P_FOLDERTXT0 = P_RINGTXT0 + kVoices,
    P_SAMPLETXT0 = P_FOLDERTXT0 + kVoices,
    P_VNAME0 = P_SAMPLETXT0 + kVoices,    // the voice's name (white), empty while it is the chosen one
    P_VNAMESEL0 = P_VNAME0 + kVoices,     // the chosen voice's name (red)
    P_ROWT0 = P_VNAMESEL0 + kVoices,
    P_PRINTROW0 = P_ROWT0 + kRows,
    P_CAP0 = P_PRINTROW0 + kPrintRows,
    // film strips
    P_CELL0 = P_CAP0 + kNumCaps,          // v * 16 + j: 0 none, 1 hit, 2 hit with a swapped sample
    P_LAMP0 = P_CELL0 + kVoices * 16,
    P_PADLIT0 = P_LAMP0 + kVoices,
    P_FLASH0 = P_PADLIT0 + kVoices,
    P_SCELL0 = P_FLASH0 + kFlashes,       // STEPS cells: v * 16 + column (144 frames, see stepTexts)
    P_PLAY0 = P_SCELL0 + kGrid,           // STEPS: the play head, per column
    P_VSEL0 = P_PLAY0 + 16,               // main page: the chosen voice's red frame (2 frames)
    P_LVLBAR0 = P_VSEL0 + kVoices,        // 0.15: main page: LEVEL as a bar (21 frames)
    P_ROWLIT0 = P_LVLBAR0 + kVoices,      // 0.15: FOLDER rows: 0 plain, 1 the voice's sample, 2 being previewed
    P_CHIPLIT0 = P_ROWLIT0 + kRows,       // 0.15: STEPS: a chip's box (0 none, 1 red)
    P_COUNT = P_CHIPLIT0 + kChips
};

// a sample in memory (16 bit)
struct Sample
{
    std::vector<int16_t> L, R; // R empty: mono
    float rate = 44100.f;
    std::string name;
    size_t frames() const { return L.size(); }
};

// what a voice can play: its folder's samples, the base one
struct Kit
{
    std::vector<Sample> samples;
    std::vector<std::string> files; // the WAV file of each sample ("" for built in / EMPTY)
    std::atomic<int> base{0};       // the voice's own sample; < > on the main page change it while it plays
};

// a voice set to EMPTY keeps this as its folder (in projects and kits)
static const char* const kEmptyFolder = "<empty>";

struct Kit;
struct Player
{
    const Sample* s = nullptr;
    const Kit* owner = nullptr; // the kit the sample belongs to (POOL plays other voices' samples)
    bool active = false, rev = false;
    double pos = 0.0, rate = 1.0, end = 0.0;
    float gain = 1.f, panL = 1.f, panR = 1.f, drive = 0.f, decayLen = 0.f;
    int bits = 16;
    float fa = 1.f, zl = 0.f, zr = 0.f;
    int age = 0, fade = -1;
};

struct Hit { double at; int voice; float vel; int step; uint32_t round; };

Sample synth(int voice, float sr)
{
    Sample s;
    s.rate = sr;
    s.name = std::string(kVoiceNames[voice]) + " (built in)";
    const float lens[kVoices] = {0.45f, 0.25f, 0.3f, 0.08f, 0.06f, 0.4f, 0.4f, 0.12f};
    size_t n = (size_t)(lens[voice] * sr);
    s.L.resize(n);
    uint32_t rng = 1234u + (uint32_t)voice * 777u;
    auto noise = [&rng]() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return (rng >> 8) * (1.f / 8388608.f) - 1.f; };
    float lp = 0.f, hp = 0.f, ph = 0.f;
    for (size_t i = 0; i < n; ++i)
    {
        float t = i / sr, v = 0.f;
        switch (voice)
        {
            case 0: ph += 2.f * 3.14159f * (45.f + 110.f * std::exp(-t * 35.f)) / sr; v = std::sin(ph) * std::exp(-t * 7.f); break;
            case 1: ph += 2.f * 3.14159f * 190.f / sr; v = (0.5f * std::sin(ph) * std::exp(-t * 25.f) + 0.6f * noise() * std::exp(-t * 14.f)); break;
            case 2: { float e = std::exp(-std::fmod(t, 0.012f) * 300.f) * (t < 0.036f ? 1.f : 0.f) + std::exp(-t * 18.f) * (t >= 0.024f ? 1.f : 0.f); lp += 0.35f * (noise() - lp); v = (noise() - lp) * e * 0.8f; break; }
            case 3: ph += 2.f * 3.14159f * 820.f / sr; v = std::sin(ph) * std::exp(-t * 60.f) * 0.7f; break;
            case 4: { float x = noise(); hp = x - lp; lp += 0.6f * (x - lp); v = hp * std::exp(-t * 70.f) * 0.6f; break; }
            case 5: { float x = noise(); hp = x - lp; lp += 0.6f * (x - lp); v = hp * std::exp(-t * 7.f) * 0.5f; break; }
            case 6: ph += 2.f * 3.14159f * (95.f + 40.f * std::exp(-t * 20.f)) / sr; v = std::sin(ph) * std::exp(-t * 8.f) * 0.8f; break;
            default: { float x = noise(); hp = x - lp; lp += 0.3f * (x - lp); v = hp * std::exp(-t * 30.f) * 0.5f; break; }
        }
        s.L[i] = (int16_t)std::lround(std::max(-1.f, std::min(1.f, v)) * 32000.f);
    }
    return s;
}

} // namespace

class Schlag : public Plugin
{
    struct KitInfo { std::string folder, file; int count = 0; }; // what a voice has loaded (worker)

public:
    Schlag() : Plugin(makeInfo()), screen_(*this)
    {
        for (int v = 0; v < kVoices; ++v) addParam(floatParam(("random" + std::to_string(v + 1)).c_str(), ("RANDOM " + std::string(kVoiceNames[v])).c_str(), 0.f, 100.f, 0.f, "%", 0));
        for (int v = 0; v < kVoices; ++v) addParam(floatParam(("level" + std::to_string(v + 1)).c_str(), ("LEVEL " + std::string(kVoiceNames[v])).c_str(), -40.f, 6.f, 0.f, "dB", 1));
        for (int v = 0; v < kVoices; ++v) addParam(boolParam(("mute" + std::to_string(v + 1)).c_str(), ("M " + std::to_string(v + 1)).c_str(), false));
        for (int v = 0; v < kVoices; ++v) addParam(boolParam(("solo" + std::to_string(v + 1)).c_str(), ("S " + std::to_string(v + 1)).c_str(), false));
        std::vector<std::string> funcs(kFuncNames, kFuncNames + NUM_FUNCS);
        for (int v = 0; v < kVoices; ++v)
            for (int k = 0; k < kSlots; ++k)
                addParam(choiceParam(("func" + std::to_string(v + 1) + "_" + std::to_string(k + 1)).c_str(), ("FUNCTION " + std::to_string(v + 1) + " " + std::to_string(k + 1)).c_str(), funcs, kDefaultFunc[k]));
        for (int v = 0; v < kVoices; ++v)
            for (int k = 0; k < kSlots; ++k)
                addParam(floatParam(("val" + std::to_string(v + 1) + "_" + std::to_string(k + 1)).c_str(), ("VALUE " + std::to_string(v + 1) + " " + std::to_string(k + 1)).c_str(), 0.f, 100.f, defaultValue(kDefaultFunc[k]), "", 0));
        for (int v = 0; v < kVoices; ++v)
            for (int f = 0; f < NUM_FUNCS; ++f)
            {
                Param q = floatParam(("fv" + std::to_string(v + 1) + "_" + kFuncNames[f]).c_str(), (std::string(kVoiceNames[v]) + " " + kFuncNames[f]).c_str(), 0.f, 100.f, defaultValue(f), "", 0);
                q.automatable = false;
                addParam(q);
            }
        addParam(floatParam("volume", "VOLUME", -40.f, 6.f, 0.f, "dB", 1));
        addParam(choiceParam("auto", "AUTO PADS", {"GENRA", "TRACK"}, 0));
        addParam(boolParam("choke", "HAT CHOKE", true));
        addParam(choiceParam("voice", "VOICE", {"KICK", "SNARE", "CLAP", "RIM", "CL HAT", "OP HAT", "TOM", "PERC"}, 0));
        addParam(boolParam("kitnotes", "MPC KIT NOTES", false));
        for (int v = 0; v < kVoices; ++v) addParam(boolParam(("live" + std::to_string(v + 1)).c_str(), ("LIVE " + std::string(kVoiceNames[v])).c_str(), false));
        {
            std::vector<std::string> kinds(kRandomKinds, kRandomKinds + NUM_RKINDS);
            for (int v = 0; v < kVoices; ++v) addParam(choiceParam(("rkind" + std::to_string(v + 1)).c_str(), ("KIND " + std::string(kVoiceNames[v])).c_str(), kinds, 0));
        }
        addParam(choiceParam("bar", "BAR", {"1", "2", "3", "4"}, 0));
        for (int i = 0; i < kLocks; ++i)
        {
            std::string n = lockName(i);
            Param q = i == LK_LEVEL ? floatParam(("step_" + n).c_str(), ("STEP " + n).c_str(), -40.f, 6.f, 0.f, "dB", 1)
                                    : floatParam(("step_" + n).c_str(), ("STEP " + n).c_str(), 0.f, 100.f, lockDefault(i), "", 0);
            q.automatable = false;
            addParam(q);
        }
        for (int i = 0; i < kQL; ++i)
        {
            const std::string n = kQLNames[i];
            std::string id = "ql_" + n;
            for (char& c : id) c = (char)::tolower((unsigned char)c);
            Param q = i == QL_LEVEL ? floatParam(id.c_str(), ("Q " + n).c_str(), -40.f, 6.f, 0.f, "dB", 1)
                    : i == QL_KIND ? choiceParam(id.c_str(), ("Q " + n).c_str(), std::vector<std::string>(kRandomKinds, kRandomKinds + NUM_RKINDS), 0)
                                   : floatParam(id.c_str(), ("Q " + n).c_str(), 0.f, 100.f, 0.f, "", 0);
            q.automatable = false;
            addParam(q);
        }
        {
            std::vector<std::string> fns(kStepFnNames, kStepFnNames + NUM_SF);
            for (int k = 0; k < kStepKnobs; ++k)
            {
                Param q = choiceParam(("stepfn" + std::to_string(k + 1)).c_str(), ("STEP FN " + std::to_string(k + 1)).c_str(), fns, kStepFnDefault[k]);
                q.automatable = false;
                addParam(q);
            }
            for (int k = 0; k < kStepKnobs; ++k)
            {
                Param q = floatParam(("stepknob" + std::to_string(k + 1)).c_str(), ("STEP KNOB " + std::to_string(k + 1)).c_str(), 0.f, 100.f, 0.f, "", 0);
                q.automatable = false;
                addParam(q);
            }
        }
        addParam(boolParam("preview", "PREVIEW", true));
        addParam(choiceParam("draw", "DRAW EDIT", {"DRAW", "EDIT"}, 0));
        addParam(boolParam("follow", "FOLLOW", false));
        for (const char* b : {"NEW", "UNDO", "BASIC", "PRINT", "LEARN", "FOLDER UP", "DRIVES", "PAGE BACK", "PAGE NEXT", "ROOT HERE", "SAVE SAMPLE", "SAVE KIT", "LOAD SAMPLE", "CLEAR STEP",
                              "SAMPLE BACK", "SAMPLE NEXT", "CONDITION BACK", "CONDITION NEXT", "HIT", "CLEAR ALL", "EMPTY", "LOAD KIT",
                              "FN BACK 1", "FN NEXT 1", "FN BACK 2", "FN NEXT 2", "COPY BAR", "PASTE", "CLEAR BAR", "DIAG", "SKIN"})
            addButton(b);
        addParam(intParam("scroll", "LIST", 0, 1000, 500));
        for (int v = 0; v < kVoices; ++v)
            for (int k = 0; k < kSlots; ++k) addButton(("FUNKTIONSWAHL " + std::to_string(v + 1) + " " + std::to_string(k + 1)).c_str());
        for (int v = 0; v < kVoices; ++v) addButton(("WAHL " + std::to_string(v + 1)).c_str());
        for (int v = 0; v < kVoices; ++v) addButton(("PAD " + std::to_string(v + 1)).c_str());
        for (int r = 0; r < kRows; ++r) addButton(("TIPP " + std::to_string(r + 1)).c_str());
        for (int v = 0; v < kVoices; ++v) addButton(("KIND TAP " + std::to_string(v + 1)).c_str());
        for (int i = 0; i < kGrid; ++i) addButton(("STEP TAP " + std::to_string(i / 16 + 1) + " " + std::to_string(i % 16 + 1)).c_str());
        for (int v = 0; v < kVoices; ++v) addButton(("LANE TAP " + std::to_string(v + 1)).c_str());
        for (int v = 0; v < kVoices; ++v) addButton(("VOICE PREV " + std::to_string(v + 1)).c_str());
        for (int v = 0; v < kVoices; ++v) addButton(("VOICE NEXT " + std::to_string(v + 1)).c_str());
        for (int v = 0; v < kVoices; ++v) addButton(("ROLL " + std::to_string(v + 1)).c_str());
        for (int i = 0; i < kSegs; ++i) addButton(("SEG TAP " + std::to_string(i + 1)).c_str());
        for (int i = 0; i < kChips; ++i) addButton(("CHIP TAP " + std::to_string(i + 1)).c_str());
        for (const char* t : {"KOPFLINK", "PFAD", "LISTINFO", "STIMMINFO", "STIMMORDNER", "STIMMSAMPLE", "STIMMANZAHL",
                              "STATUS", "PRINTINFO", "PRINTSTATUS", "PRINTORDNER", "LERNINFO", "BELEGUNG", "HAUPTORDNER",
                              "LINKINFO", "SPEICHERINFO", "VERSION", "STEPKOPF", "STEPPOS", "STEPSAMPLE", "STEPCOND", "QLTEXT", "KITTEXT", "SEITE", "DIAGINFO",
                              "STEPFN 1", "STEPFN 2", "STEPFNWERT 1", "STEPFNWERT 2"})
            addText(t);
        for (int i = 0; i < kLocks; ++i) addText(("STEPWERT " + std::to_string(i + 1)).c_str());
        for (int i = 0; i < kSegs; ++i) addText(("SEGTEXT " + std::to_string(i + 1)).c_str());
        for (int i = 0; i < kChips; ++i) addText(("CHIPTEXT " + std::to_string(i + 1)).c_str());
        for (int i = 0; i < 4; ++i) addText(("BARPUNKTE " + std::to_string(i + 1)).c_str());
        for (int v = 0; v < kVoices; ++v) addText(("MEHR " + std::to_string(v + 1)).c_str());
        for (int v = 0; v < kVoices; ++v) addText(("RINGTEXT " + std::to_string(v + 1)).c_str());
        for (int v = 0; v < kVoices; ++v) addText(("ORDNERTEXT " + std::to_string(v + 1)).c_str());
        for (int v = 0; v < kVoices; ++v) addText(("SAMPLETEXT " + std::to_string(v + 1)).c_str());
        for (int v = 0; v < kVoices; ++v) addText(("STIMMNAME " + std::to_string(v + 1)).c_str());
        for (int v = 0; v < kVoices; ++v) addText(("STIMMNAME ROT " + std::to_string(v + 1)).c_str());
        for (int r = 0; r < kRows; ++r) addText(("ZEILE " + std::to_string(r + 1)).c_str());
        for (int r = 0; r < kPrintRows; ++r) addText(("PRINTZEILE " + std::to_string(r + 1)).c_str());
        for (const auto& c : kCaptions) addText((std::string("CAP ") + c[0]).c_str());
        for (int v = 0; v < kVoices; ++v)
            for (int j = 0; j < 16; ++j) addText(("ZELLE " + std::to_string(v + 1) + " " + std::to_string(j + 1)).c_str());
        for (int v = 0; v < kVoices; ++v) addText(("LAMPE " + std::to_string(v + 1)).c_str());
        for (int v = 0; v < kVoices; ++v) addText(("PADLICHT " + std::to_string(v + 1)).c_str());
        for (int i = 0; i < kFlashes; ++i) addText(("BLITZ " + std::to_string(i + 1)).c_str());
        for (int i = 0; i < kGrid; ++i) addText(("SCHRITT " + std::to_string(i / 16 + 1) + " " + std::to_string(i % 16 + 1)).c_str());
        for (int j = 0; j < 16; ++j) addText(("KOPFSPALTE " + std::to_string(j + 1)).c_str());
        for (int v = 0; v < kVoices; ++v) addText(("STIMMRAHMEN " + std::to_string(v + 1)).c_str());
        for (int v = 0; v < kVoices; ++v) addText(("LEVELBALKEN " + std::to_string(v + 1)).c_str());
        for (int r = 0; r < kRows; ++r) addText(("ZEILENLICHT " + std::to_string(r + 1)).c_str());
        for (int i = 0; i < kChips; ++i) addText(("CHIPLICHT " + std::to_string(i + 1)).c_str());
        if (numParams() != P_COUNT) std::abort();

        screen_.init();
        for (int i = P_HDRLINK; i < P_CELL0; ++i) screen_.text(i);
        for (int i = 0; i < kNumCaps; ++i) screen_.text(P_CAP0 + i, kCaptions[i][1]);
        for (int i = 0; i < kVoices * 16; ++i) screen_.strip(P_CELL0 + i, 6); // 0.15: +3 = the step that plays
        for (int v = 0; v < kVoices; ++v) { screen_.strip(P_LAMP0 + v, 2); screen_.strip(P_PADLIT0 + v, 2); }
        for (int i = 0; i < kFlashes; ++i) screen_.button(P_NEW + i, P_FLASH0 + i);
        for (int i = P_FUNCTAP0; i < P_VNEXT0 + kVoices; ++i) if (i < P_PADTAP0 || i >= P_ROWTAP0) screen_.button(i);
        for (int i = 0; i < kGrid; ++i) screen_.strip(P_SCELL0 + i, 144);
        for (int j = 0; j < 16; ++j) screen_.strip(P_PLAY0 + j, 2);
        for (int v = 0; v < kVoices; ++v) screen_.strip(P_VSEL0 + v, 2);
        for (int v = 0; v < kVoices; ++v) screen_.strip(P_LVLBAR0 + v, kLvlFrames);
        for (int r = 0; r < kRows; ++r) screen_.strip(P_ROWLIT0 + r, 3);
        for (int i = 0; i < kChips; ++i) screen_.strip(P_CHIPLIT0 + i, 2);
        for (int i = P_ROLL0; i < P_CHIPTAP0 + kChips; ++i) screen_.button(i);
        screen_.watch(P_BAR);
        screen_.watch(P_SCROLL);
        screen_.watch(P_AUTO);
        screen_.setText(P_VERSION, "GENRA SCHLAG 0.16 · LINK v3");

        for (int v = 0; v < kVoices; ++v)
        {
            kit_[v].store(nullptr);
            pendingKit_[v].store(nullptr);
            for (auto& r : retired_[v]) r.store(nullptr);
            seed_[v].store(1u + (uint32_t)v * 97u);
            for (auto& st : lock_[v]) for (auto& x : st) x.store(std::numeric_limits<float>::quiet_NaN());
            for (auto& x : smpLock_[v]) x.store(-1);
            for (auto& x : condLock_[v]) x.store(0);
            swapped_[v].store(0);
            holdRound_[v].store(0);
            lastRound_[v].store(0);
            liveStable_[v].store(false);
            seenMask_[v].store(0);
            seenLoad_[v].store(0);
            seenLoadReq_[v].store(false);
        }
        preview_.store(nullptr);
        for (auto& r : previewRetired_) r.store(nullptr);
        for (int& f : shownFunc_) f = -1;
    }

    ~Schlag() override
    {
        quit_.store(true);
        if (worker_.joinable()) worker_.join();
        for (int v = 0; v < kVoices; ++v)
        {
            delete kit_[v].exchange(nullptr);
            delete pendingKit_[v].exchange(nullptr);
            for (auto& r : retired_[v]) delete r.exchange(nullptr);
        }
        delete preview_.exchange(nullptr);
        delete previewPending_.exchange(nullptr);
        for (auto& r : previewRetired_) delete r.exchange(nullptr);
    }

    static PluginInfo makeInfo()
    {
        PluginInfo i;
        i.name = "GENRA SCHLAG";
        i.vendor = "Genra";
        i.uniqueId = MPC_FOURCC('G', 'n', 'S', 'g');
        i.version = 800;
        i.isInstrument = true;
        i.numInputs = 0;
        i.numOutputs = 2;
        return i;
    }

    void prepare(double sampleRate, int) override
    {
        sr_ = sampleRate;
        if (!started_)
        {
            started_ = true;
            link_.open();
            pads_.load();
            settings_.load();
            for (int v = 0; v < kVoices; ++v)
            {
                Kit* k = new Kit();
                k->samples.push_back(synth(v, (float)sampleRate));
                kitInfo_[v] = {std::string(), "built in", 1};
                delete pendingKit_[v].exchange(k);
            }
            worker_ = std::thread([this] { workerMain(); });
        }
    }

    void reset() override
    {
        for (auto& p : players_) for (auto& q : p) q.active = false;
        nHits_ = 0; wasPlaying_ = false; gainSmooth_ = -1.f; limGain_ = 1.f;
        for (int v = 0; v < kVoices; ++v) padSeen_[v] = getInt(P_PADTAP0 + v);
    }

    void process(ProcessContext& ctx) override
    {
        for (int v = 0; v < kVoices; ++v)
            if (Kit* k = pendingKit_[v].exchange(nullptr, std::memory_order_acq_rel))
            {
                Kit* old = kit_[v].exchange(k);
                for (auto& q : players_[v]) q.active = false;
                // POOL: other voices may play a sample of the old kit
                for (auto& pv : players_) for (auto& q : pv) if (q.owner == old) q.active = false;
                if (old) retire(old, retired_[v]);
            }
        if (Sample* p = previewPending_.exchange(nullptr, std::memory_order_acq_rel))
        {
            if (Sample* old = preview_.exchange(p)) retire(old, previewRetired_);
            previewPos_ = 0.0;
        }
        genralink::LinkData* L = link_.data();
        if (L)
        {
            L->lane[genralink::LANE_DRUMS].beat.fetch_add(1, std::memory_order_relaxed);
            L->drumsTrack.store(getInt(P_AUTO) == 1 ? 1 : 0, std::memory_order_relaxed); // TRACK: GENRA CHOP sends no drums
        }
        // the loop that plays: a new one from GENRA CHOP takes over at the end of the pattern (or at once when stopped)
        const Transport& tr0 = ctx.transport;
        // 0.16 DIAG: what the host reports, for the capture (genra_diag.h)
        diagTr_.ppq.store(tr0.ppqPos, std::memory_order_relaxed);
        diagTr_.samplePos.store(tr0.samplePos, std::memory_order_relaxed);
        diagTr_.tempo.store(tr0.bpm, std::memory_order_relaxed);
        diagTr_.barStart.store(tr0.barStartPpq, std::memory_order_relaxed);
        diagTr_.flags.store((tr0.valid ? 1 : 0) | (tr0.playing ? 2 : 0) | (tr0.recording ? 4 : 0) | (tr0.ppqValid ? 8 : 0) |
                                (tr0.cycling ? 16 : 0), std::memory_order_relaxed);
        // the MPC's count-in: silent until the one (like GENRA CHOP 0.12: a position before the start, a negative sample
        // position or a position that stands still while "playing")
        const bool raw0 = tr0.valid && tr0.playing && tr0.ppqValid;
        const bool frozen0 = raw0 && rawWas_ && std::fabs(tr0.ppqPos - rawLastQ_) < 1e-9;
        rawWas_ = raw0;
        rawLastQ_ = tr0.ppqPos;
        const double bpb0 = tr0.timeSigDenominator > 0 ? 4.0 * tr0.timeSigNumerator / tr0.timeSigDenominator : 4.0;
        const bool pastBar0 = tr0.recording && tr0.ppqPos - tr0.barStartPpq > bpb0 + 0.5; // the MPC's count-in (DIAG)
        countIn_ = tr0.ppqPos < -0.01 || tr0.samplePos < -1.0 || frozen0 || pastBar0;
        const bool playing0 = raw0 && !countIn_;
        // a hand edit (STEPS: HIT ON / OFF) is taken at once; a new loop at the end of the pattern
        if (cur_ < 0 || !playing0 || readyNow_.exchange(false)) { int r = ready_.load(std::memory_order_acquire); if (r >= 0) cur_ = r; }
        // STEPS: a sample chosen for a step plays once to listen
        if (int a = audition_.exchange(0))
        {
            int v = (a - 1) / 1024, idx = (a - 1) % 1024;
            if (v >= 0 && v < kVoices) triggerSample(v, idx);
        }
        if (cur_ >= 0) inUse_.store(cur_, std::memory_order_release);
        const genralink::Loop* loop = cur_ >= 0 ? &loops_[cur_] : nullptr;
        long len = L ? (long)L->loopSteps.load(std::memory_order_relaxed) : 0;
        const long start = L && len >= 4 && len <= 1024 ? (long)L->loopStart.load(std::memory_order_relaxed) : 0;
        if (len < 4 || len > 1024) len = kSteps;
        // RANDOM: the positions that count (the loop's hits, the track's notes of this and the last pass)
        autoNow_ = getInt(P_AUTO) == 0;
        for (int v = 0; v < kVoices; ++v)
        {
            uint64_t m = 0;
            if (loop) for (int st = 0; st < kSteps; ++st) if (loop->drum[v][st].vel) m |= 1ull << st;
            loopMask_[v] = m;
            if (seenLoadReq_[v].exchange(false, std::memory_order_acq_rel)) { seenPrev_[v] = seenLoad_[v].load(); seenCur_[v] = 0; }
            seenMask_[v].store(seenCur_[v] | seenPrev_[v], std::memory_order_relaxed);
        }
        // the MPC jumps back at the end of its sequence: count the passes so LIVE rolls again on every one
        if (playing0 && lastPpq_ >= 0.0 && tr0.ppqPos < lastPpq_ - 0.25) ++wraps_;
        lastPpq_ = playing0 ? tr0.ppqPos : -1.0;

        for (int v = 0; v < kVoices; ++v)
        {
            int t = getInt(P_PADTAP0 + v);
            if (t != padSeen_[v]) { padSeen_[v] = t; trigger(v, 0.85f, -1, 0); }
        }

        const Transport& tr = ctx.transport;
        const bool playing = tr.valid && tr.playing && tr.ppqValid && !countIn_;
        const bool autoMode = getInt(P_AUTO) == 0;
        const double bpm = tr.tempoValid && tr.bpm > 1.0 ? tr.bpm : 120.0;
        const double spStep = sr_ * 60.0 / bpm / 4.0;
        if (playing && autoMode && cur_ >= 0)
        {
            const double q0 = tr.ppqPos * 4.0, q1 = q0 + ctx.numFrames / spStep;
            if (!wasPlaying_ || nextStep_ < q0 - 1.0 || nextStep_ > q1 + 1.0)
            {
                nextStep_ = std::floor(q0 + 1e-6); // a step that began a moment ago (start, end of the count-in) still plays
                if (q0 - nextStep_ > 0.25) nextStep_ += 1.0;
            }
            const float swing = loop->swingX1000 / 1000.f;
            while (nextStep_ < q1)
            {
                long n = (long)std::llround(nextStep_) - start;
                long pos = ((n % len) + len) % len;
                int s = (int)(pos % kSteps);
                if (pos == 0)
                {
                    int r = ready_.load(std::memory_order_acquire);
                    if (r >= 0 && r != cur_) { cur_ = r; inUse_.store(cur_, std::memory_order_release); loop = &loops_[cur_]; }
                }
                uint32_t round = (uint32_t)((n - pos) / len) + wraps_ * 7919u; // the pass of the loop
                double base = std::max(0.0, (nextStep_ - q0) * spStep + ((s & 1) ? (swing - 0.5f) * 2.f * spStep : 0.0));
                const uint32_t pass = (uint32_t)((n - pos) / len);
                for (int v = 0; v < kVoices; ++v)
                {
                    const genralink::DrumHit& h = loop->drum[v][s];
                    if (!h.vel) continue;
                    if (!stepPlays(v, s, pass)) continue; // STEPS: the step's own CHANCE / CONDITION
                    if (h.cond == 1 && (round & 1u)) continue;
                    if (h.chance < 255 && ((uint32_t)(s * 131 + v * 17 + round * 7919) * 2654435761u >> 24) > h.chance) continue;
                    int rat = std::max(1, (int)h.ratchet);
                    int rep = repeatOf(v, s);
                    if (rep > 1) rat = std::max(rat, rep);
                    double at = base + (h.off > 0 ? h.off / 100.0 * spStep : 0.0);
                    for (int k = 0; k < rat && nHits_ < 64; ++k)
                        hits_[nHits_++] = {at + k * spStep / rat, v, h.vel / 127.f * (k ? 0.75f : 1.f), s, round};
                }
                if (s == 0) for (auto& sw : swapped_) sw.store(0, std::memory_order_relaxed); // a new pass: new marks
                curStep_.store(s, std::memory_order_relaxed);
                nextStep_ += 1.0;
            }
            wasPlaying_ = true;
        }
        else
        {
            wasPlaying_ = false;
            nHits_ = 0;
            curStep_.store(playing ? (int)((((long)std::floor(tr.ppqPos * 4.0) % kSteps) + kSteps) % kSteps) : -1);
        }

        // the controls, once per block
        bool solo = false;
        for (int v = 0; v < kVoices; ++v) if (getBool(P_SOLO0 + v)) solo = true;
        for (int v = 0; v < kVoices; ++v)
        {
            audible_[v] = !getBool(P_MUTE0 + v) && (!solo || getBool(P_SOLO0 + v));
            levelGain_[v] = std::pow(10.f, get(P_LEVEL0 + v) / 20.f);
        }
        float target = std::pow(10.f, get(P_VOLUME) / 20.f);
        if (gainSmooth_ < 0.f) gainSmooth_ = target;
        const float limRel = std::exp(-1.f / (float)(sr_ * 0.12));
        const float ceiling = 0.966f; // -0.3 dB
        float* outL = ctx.outputs[0];
        float* outR = ctx.numOutputs > 1 ? ctx.outputs[1] : nullptr;
        int ev = 0;
        uint32_t lit = 0;
        const bool kitNotes = getBool(P_KITNOTES);
        for (int i = 0; i < ctx.numFrames; ++i)
        {
            while (ev < ctx.numMidiEvents && ctx.midi[ev].sampleOffset <= i)
            {
                const MidiEvent& e = ctx.midi[ev++];
                if (!e.isNoteOn()) continue;
                if (pads_.learnNote(e.note())) continue;
                // channel 10 (GENRA CHOP OUT, MPC drum notes) or MPC KIT NOTES: A01-A08 = the voices; else the pad map
                int pad = (e.channel() == 9 || kitNotes) ? kitVoice(e.note()) : pads_.padOf(e.note());
                if (pad < 0 || pad >= kVoices) continue;
                // GENRA: a note on channel 10 where GENRA's loop has this voice's hit is GENRA CHOP OUT coming back
                // (its own drums, e.g. while the track records them): LINK plays it already. Pads off the loop still play.
                if (autoMode && playing && e.channel() == 9 && cur_ >= 0)
                {
                    long n = (long)std::llround((tr.ppqPos + (double)i / (spStep * 4.0)) * 4.0) - start;
                    long pos = ((n % len) + len) % len;
                    const int s0 = (int)(pos % kSteps);
                    bool echo = false;
                    for (int d = -1; d <= 1 && !echo; ++d) echo = loop->drum[pad][(s0 + d + kSteps) % kSteps].vel > 0;
                    if (echo) continue;
                }
                // a note of the MPC track: its position (16th 0-63 in 4 bars) decides its sample, like a loop hit
                int step = -1;
                uint32_t round = 0;
                if (playing)
                {
                    long q = (long)std::llround((tr.ppqPos + (double)i / (spStep * 4.0)) * 4.0);
                    step = (int)(((q % kSteps) + kSteps) % kSteps);
                    round = (uint32_t)((q - step) / kSteps) + wraps_ * 7919u;
                    if (round != midiRound_[pad])
                    {
                        if (seenCur_[pad]) seenPrev_[pad] = seenCur_[pad];
                        seenCur_[pad] = 0;
                        midiRound_[pad] = round;
                    }
                    seenCur_[pad] |= 1ull << step;
                }
                trigger(pad, e.velocity() / 127.f, step, round);
            }
            for (int k = 0; k < nHits_;)
            {
                if (hits_[k].at <= (double)i) { trigger(hits_[k].voice, hits_[k].vel, hits_[k].step, hits_[k].round); hits_[k] = hits_[--nHits_]; }
                else ++k;
            }
            float l = 0.f, rr = 0.f;
            for (int v = 0; v < kVoices; ++v)
                for (Player& p : players_[v])
                {
                    if (!p.active) continue;
                    float a, b;
                    if (!sampleAt(p, a, b)) { p.active = false; continue; }
                    float env = p.gain;
                    if (p.decayLen > 0.f) env *= std::max(0.f, 1.f - p.age / p.decayLen);
                    if (p.age < 16) env *= p.age / 16.f;
                    if (p.fade >= 0) { env *= p.fade / 64.f; if (--p.fade <= 0) { p.active = false; continue; } }
                    if (p.bits < 16) { float q = (float)(1 << (p.bits - 1)); a = std::round(a * q) / q; b = std::round(b * q) / q; }
                    if (p.drive > 0.01f) { float d = 1.f + 8.f * p.drive; a = std::tanh(a * d) / std::tanh(d); b = std::tanh(b * d) / std::tanh(d); }
                    if (p.fa < 1.f) { p.zl += p.fa * (a - p.zl); p.zr += p.fa * (b - p.zr); a = p.zl; b = p.zr; }
                    if (audible_[v]) { l += a * env * p.panL; rr += b * env * p.panR; lit |= 1u << v; }
                    p.pos += p.rev ? -p.rate : p.rate;
                    ++p.age;
                }
            if (Sample* pv = preview_.load(std::memory_order_relaxed))
            {
                size_t k = (size_t)previewPos_;
                if (k + 1 < pv->frames())
                {
                    float a = pv->L[k] / 32768.f, b = pv->R.empty() ? a : pv->R[k] / 32768.f;
                    l += a * 0.5f; rr += b * 0.5f;
                    previewPos_ += pv->rate / sr_;
                }
            }
            gainSmooth_ += (target - gainSmooth_) * 0.002f;
            l *= gainSmooth_; rr *= gainSmooth_;
            float peak = std::max(std::fabs(l), std::fabs(rr));
            float need = peak > ceiling ? ceiling / peak : 1.f;
            limGain_ = need < limGain_ ? need : 1.f - (1.f - limGain_) * limRel;
            outL[i] = l * limGain_;
            if (outR) outR[i] = rr * limGain_;
        }
        for (int k = 0; k < nHits_; ++k) hits_[k].at -= ctx.numFrames;
        litMask_.store(lit, std::memory_order_relaxed);
        // inUse_ keeps cur_: the worker must not overwrite the loop that plays
    }

    void saveExtraState(std::vector<uint8_t>& out) override
    {
        std::lock_guard<std::mutex> g(mtx_);
        // SG2: per voice folder, file, then RANDOM's roll (seed, HOLD's pass, the track's note positions)
        std::string s = "SG2\n";
        for (int v = 0; v < kVoices; ++v)
        {
            char b[64];
            std::snprintf(b, sizeof b, "\t%u\t%u\t%llx", seed_[v].load(), holdRound_[v].load(), (unsigned long long)seenMask_[v].load());
            s += voiceFolder_[v] + "\t" + voiceFile_[v] + b + "\n";
        }
        // STEPS: the steps' own values (voice, step, which, value)
        s += "LOCKS\n";
        for (int v = 0; v < kVoices; ++v)
            for (int st = 0; st < kSteps; ++st)
                for (int i = 0; i < kLocks; ++i)
                {
                    float x = lock_[v][st][i].load();
                    if (std::isnan(x)) continue;
                    char b[48];
                    std::snprintf(b, sizeof b, "%d %d %d %.2f\n", v, st, i, x);
                    s += b;
                }
        // a step's own sample (100) and CONDITION (101), in the same line format
        for (int v = 0; v < kVoices; ++v)
            for (int st = 0; st < kSteps; ++st)
            {
                char b[48];
                if (smpLock_[v][st].load() >= 0) { std::snprintf(b, sizeof b, "%d %d 100 %d\n", v, st, (int)smpLock_[v][st].load()); s += b; }
                if (condLock_[v][st].load() > 0) { std::snprintf(b, sizeof b, "%d %d 101 %d\n", v, st, (int)condLock_[v][st].load()); s += b; }
            }
        out.assign(s.begin(), s.end());
    }

    void loadExtraState(const uint8_t* data, size_t size) override
    {
        std::lock_guard<std::mutex> g(mtx_);
        std::string s(reinterpret_cast<const char*>(data), size);
        const bool v2 = s.compare(0, 4, "SG2\n") == 0;
        if (!v2 && s.compare(0, 4, "SG1\n") != 0) return;
        for (auto& a : lock_) for (auto& b : a) for (auto& c : b) c.store(std::numeric_limits<float>::quiet_NaN());
        for (auto& a : smpLock_) for (auto& b : a) b.store(-1);
        for (auto& a : condLock_) for (auto& b : a) b.store(0);
        size_t lk = s.find("\nLOCKS\n");
        if (lk != std::string::npos)
        {
            std::string t = s.substr(lk + 7);
            s.resize(lk + 1);
            int v, st, i;
            float x;
            const char* c = t.c_str();
            int n = 0;
            while (std::sscanf(c, "%d %d %d %f\n%n", &v, &st, &i, &x, &n) == 4 && n > 0)
            {
                if (v >= 0 && v < kVoices && st >= 0 && st < kSteps)
                {
                    if (i >= 0 && i < kLocks) lock_[v][st][i].store(x);
                    else if (i == 100) smpLock_[v][st].store((int16_t)clamp((int)x, -1, kMaxFiles - 1));
                    else if (i == 101) condLock_[v][st].store((int8_t)clamp((int)x, 0, NUM_CONDS - 1));
                }
                c += n;
                n = 0;
            }
        }
        stepSync_.store(true);
        size_t pos = 4;
        for (int v = 0; v < kVoices && pos < s.size(); ++v)
        {
            size_t e = s.find('\n', pos);
            std::string line = s.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
            pos = e == std::string::npos ? s.size() : e + 1;
            std::vector<std::string> f;
            for (size_t a = 0;;)
            {
                size_t t = line.find('\t', a);
                f.push_back(line.substr(a, t == std::string::npos ? std::string::npos : t - a));
                if (t == std::string::npos) break;
                a = t + 1;
            }
            voiceFolder_[v] = f[0];
            voiceFile_[v] = f.size() > 1 ? f[1] : std::string();
            if (v2 && f.size() >= 5)
            {
                seed_[v].store((uint32_t)std::strtoul(f[2].c_str(), nullptr, 10));
                holdRound_[v].store((uint32_t)std::strtoul(f[3].c_str(), nullptr, 10));
                seenLoad_[v].store((uint64_t)std::strtoull(f[4].c_str(), nullptr, 16));
                seenLoadReq_[v].store(true);
            }
            loadReq_[v] = true;
        }
        resyncReq_.store(true);
        randomSync_.store(true);
    }

    bool formatValue(int index, float plain, std::string& out) override
    {
        char b[32];
        if (index >= P_RANDOM0 && index < P_RANDOM0 + kVoices) { std::snprintf(b, sizeof b, "%.0f %%", plain); out = b; return true; }
        if (index >= P_LIVE0 && index < P_LIVE0 + kVoices) { out = plain >= 0.5f ? "LIVE" : "HOLD"; return true; }
        if (index >= P_RKIND0 && index < P_RKIND0 + kVoices) { out = kRandomKinds[clamp((int)std::lround(plain), 0, NUM_RKINDS - 1)]; return true; }
        if (index >= P_SLK0 && index < P_SLK0 + kLocks) { out = lockText(index - P_SLK0, plain); return true; }
        if (index >= P_QL0 && index < P_QL0 + kQL)
        {
            const int i = index - P_QL0, v = clamp(qlVoice_, 0, kVoices - 1);
            if (i == QL_SAMPLE)
            {
                Kit* k = kit_[v].load(std::memory_order_acquire);
                const int n = k ? (int)k->samples.size() : 0;
                if (n > 0) std::snprintf(b, sizeof b, "%d/%d", sampleOfKnob(plain, n) + 1, n);
                else std::snprintf(b, sizeof b, "no folder");
                out = b;
            }
            else if (i == QL_RANDOM) { std::snprintf(b, sizeof b, "%.0f %%", plain); out = b; }
            else if (i == QL_LEVEL) { std::snprintf(b, sizeof b, "%+.1f dB", plain); out = b; }
            else if (i == QL_KIND) out = kRandomKinds[clamp((int)std::lround(plain), 0, NUM_RKINDS - 1)];
            else out = valueText(clamp(getInt(P_FUNC0 + v * kSlots + (i - QL_UPPER)), 0, NUM_FUNCS - 1), plain);
            return true;
        }
        if (index >= P_SKN0 && index < P_SKN0 + kStepKnobs) { out = stepKnobText(index - P_SKN0); return true; }
        if (index >= P_LEVEL0 && index < P_LEVEL0 + kVoices) { std::snprintf(b, sizeof b, "%+.1f dB", plain); out = b; return true; }
        if (index >= P_FUNC0 && index < P_FUNC0 + kVoices * kSlots)
        {
            // 0.15: the function's name; "+n" beside it counts the others off neutral (MEHR)
            out = kFuncNames[clamp((int)std::lround(plain), 0, NUM_FUNCS - 1)];
            return true;
        }
        if (index >= P_VAL0 && index < P_VAL0 + kVoices * kSlots)
        {
            int f = clamp(getInt(P_FUNC0 + (index - P_VAL0)), 0, NUM_FUNCS - 1);
            out = valueText(f, plain);
            return true;
        }
        if (index >= P_CELL0) return false;
        return screen_.format(index, out);
    }

private:
    static float defaultValue(int f)
    {
        switch (f) { case F_TUNE: case F_PAN: return 50.f; case F_DECAY: case F_BITS: case F_FILTER: return 100.f; default: return 0.f; }
    }
    static std::string valueText(int f, float v)
    {
        char b[32];
        switch (f)
        {
            case F_TUNE: std::snprintf(b, sizeof b, "%+d st", (int)std::lround((v - 50.f) * 0.24f)); break;
            case F_DECAY: std::snprintf(b, sizeof b, v >= 99.5f ? "full" : "%.0f %%", v); break;
            case F_DRIVE: std::snprintf(b, sizeof b, "%.0f %%", v); break;
            case F_BITS: std::snprintf(b, sizeof b, "%d", bitsOf(v)); break;
            case F_REPEAT: { const char* r[4] = {"off", "1/32", "1/48", "1/64"}; std::snprintf(b, sizeof b, "%s", r[repeatIndex(v)]); break; }
            case F_PAN: { int p = (int)std::lround((v - 50.f) * 2.f); if (!p) std::snprintf(b, sizeof b, "Center"); else std::snprintf(b, sizeof b, "%s %d", p < 0 ? "L" : "R", std::abs(p)); break; }
            case F_REVERSE: std::snprintf(b, sizeof b, v >= 50.f ? "on" : "off"); break;
            default: if (v >= 99.5f) std::snprintf(b, sizeof b, "open"); else std::snprintf(b, sizeof b, "LP %.1fk", cutoffHz(v) / 1000.f); break;
        }
        return b;
    }
    static std::string lockName(int i)
    {
        if (i == 0) return "RANDOM";
        if (i == LK_LEVEL) return "LEVEL";
        if (i == LK_CHANCE) return "CHANCE";
        return kFuncNames[i - 1];
    }
    static float lockDefault(int i) { return i == 0 ? 0.f : i == LK_LEVEL ? 0.f : i == LK_CHANCE ? 100.f : defaultValue(i - 1); }
    static std::string lockText(int i, float x)
    {
        char b[32];
        if (i == 0 || i == LK_CHANCE) std::snprintf(b, sizeof b, "%.0f %%", x);
        else if (i == LK_LEVEL) std::snprintf(b, sizeof b, "%+.1f dB", x);
        else return valueText(i - 1, x);
        return b;
    }
    static int kitVoice(int note)
    {
        for (int v = 0; v < kVoices; ++v) if (kKitNote[v] == note) return v;
        return -1;
    }
    static int bitsOf(float v) { return clamp((int)std::lround(4.f + v * 0.12f), 4, 16); }
    static int repeatIndex(float v) { return clamp((int)(v / 25.f), 0, 3); }
    static float cutoffHz(float v) { return 150.f * std::pow(2.f, v / 100.f * 7.f); }

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

    template <typename T, size_t N>
    static void retire(T* s, std::atomic<T*> (&bin)[N])
    {
        for (auto& r : bin)
        {
            T* expected = nullptr;
            if (r.compare_exchange_strong(expected, s)) return;
        }
    }

    // ------------------------------------------------------------------ audio
    // every function keeps its value, shown in a slot or not
    float funcValue(int v, int f, float) const { return get(P_FV0 + v * NUM_FUNCS + f); }
    // REPEAT: hits per step (aus, 1/32 = 2, 1/48 = 3, 1/64 = 4)
    int repeatOf(int v, int step = -1) const { return repeatIndex(fvAt(v, step, F_REPEAT)) + 1; }

    // the sample a hit plays: the base one, or (RANDOM) another one from the folder.
    // RANDOM = the share of the voice's hits (positions 0-63 in 4 bars) that swap: exactly round(pct * hits) of them,
    // the ones with the lowest roll. HOLD keeps the roll (it changes only when RANDOM moves); LIVE rolls every pass.
    static uint32_t mix(uint32_t h) { h ^= h >> 15; h *= 0x2c1b3c6du; h ^= h >> 12; h *= 0x297a2d39u; h ^= h >> 15; return h; }
    uint32_t rollOf(int v, int pos, uint32_t seed, uint32_t r) const
    {
        return mix((uint32_t)pos * 2654435761u ^ seed * 40503u ^ (uint32_t)v * 9176u ^ r * 0x9e3779b9u);
    }
    // STEPS: a step's own RANDOM / function value, NaN = none (the main page's value counts)
    float lockAt(int v, int step, int i) const
    {
        if (step < 0) return std::numeric_limits<float>::quiet_NaN();
        return lock_[v][step & (kSteps - 1)][i].load(std::memory_order_relaxed);
    }
    // a function's value for a hit: the step's own, else the voice's
    float fvAt(int v, int step, int f) const
    {
        float x = lockAt(v, step, 1 + f);
        return std::isnan(x) ? funcValue(v, f, 0.f) : x;
    }

    // does RANDOM pick this hit? (the share of the voice's hits, or the step's own RANDOM as a chance)
    bool picks(int v, int step, uint32_t round, uint64_t mask, uint32_t& roll) const
    {
        if (step < 0) return false;
        step &= kSteps - 1;
        const uint32_t seed = seed_[v].load(std::memory_order_relaxed);
        const uint32_t r = liveStable_[v].load(std::memory_order_relaxed) ? round : holdRound_[v].load(std::memory_order_relaxed);
        const uint32_t mine = rollOf(v, step, seed, r);
        roll = mine;
        float own = lockAt(v, step, 0);
        if (!std::isnan(own)) return own > 0.f && (double)mine < own / 100.0 * 4294967296.0;
        float pct = get(P_RANDOM0 + v) / 100.f;
        if (pct <= 0.f) return false;
        mask |= 1ull << step;
        const int total = __builtin_popcountll(mask);
        const int count = std::min(total, (int)std::lround(pct * (float)total));
        if (count <= 0) return false;
        int below = 0;
        for (uint64_t m = mask & ~(1ull << step); m; m &= m - 1)
        {
            int p = __builtin_ctzll(m);
            uint32_t o = rollOf(v, p, seed, r);
            if (o < mine || (o == mine && p < step)) ++below;
        }
        return below < count;
    }
    static float frac(uint32_t roll, uint32_t salt) { return (float)(mix(roll ^ salt) % 10000u) / 10000.f; }

    // the voice's positions that count for RANDOM: the loop's hits (AUTO) and the positions of the track's notes
    uint64_t hitMask(int v) const { return (autoNow_ ? loopMask_[v] : 0ull) | seenCur_[v] | seenPrev_[v]; }

    // STEPS: does the step play this pass? (its own CHANCE and CONDITION; none = it plays)
    bool stepPlays(int v, int step, uint32_t pass) const
    {
        step &= kSteps - 1;
        switch (condLock_[v][step].load(std::memory_order_relaxed))
        {
            case 1: if (pass % 2u != 0) return false; break;
            case 2: if (pass % 2u != 1) return false; break;
            case 3: case 4: case 5: case 6: if (pass % 4u != (uint32_t)(condLock_[v][step].load() - 3)) return false; break;
            case 7: if (pass == 0) return false; break;
            default: break;
        }
        float c = lockAt(v, step, LK_CHANCE);
        if (std::isnan(c) || c >= 100.f) return true;
        uint32_t h = mix((uint32_t)step * 2246822519u ^ (uint32_t)v * 3266489917u ^ pass * 668265263u ^ 0x5bd1e995u);
        return (double)h < c / 100.0 * 4294967296.0;
    }

    // STEPS: listen to a sample of the voice's folder (the editor's SAMPLE < >)
    void triggerSample(int v, int idx)
    {
        Kit* k = kit_[v].load(std::memory_order_acquire);
        if (!k || k->samples.empty()) return;
        const Sample& s = k->samples[(size_t)clamp(idx, 0, (int)k->samples.size() - 1)];
        if (!s.frames()) return;
        for (Player& q : players_[v]) if (q.active && q.fade < 0) q.fade = 64;
        Player* p = &players_[v][0];
        for (Player& q : players_[v]) if (!q.active) { p = &q; break; }
        *p = Player();
        p->s = &s;
        p->owner = k;
        p->rate = s.rate / sr_;
        p->end = (double)s.frames() - 1.0;
        p->gain = 0.8f * levelGain_[v];
        p->active = true;
    }

    void trigger(int v, float vel, int step, uint32_t round)
    {
        Kit* k = kit_[v].load(std::memory_order_acquire);
        if (!k || k->samples.empty()) return;
        uint32_t roll = 0;
        const bool pk = picks(v, step, round, hitMask(v), roll);
        if (step >= 0) lastRound_[v].store(liveStable_[v].load(std::memory_order_relaxed) ? round : holdRound_[v].load(std::memory_order_relaxed), std::memory_order_relaxed);
        const int kind = pk ? clamp(getInt(P_RKIND0 + v), 0, NUM_RKINDS - 1) : -1;
        // the sample: SAMPLE = another one of the voice's folder, POOL = any of all eight voices' samples
        const Kit* kk = k;
        const int base = k->base.load(std::memory_order_relaxed);
        int si = base;
        // STEPS: a step's own sample (from the voice's folder); RANDOM then does not swap the sample there
        const int own = step >= 0 ? smpLock_[v][step & (kSteps - 1)].load(std::memory_order_relaxed) : -1;
        if (own >= 0 && own < (int)k->samples.size()) si = own;
        else if (kind == RK_SAMPLE && k->samples.size() > 1)
        {
            int n = (int)k->samples.size(), alt = (int)(mix(roll ^ 0x85ebca6bu) % (uint32_t)(n - 1));
            si = alt >= base ? alt + 1 : alt;
        }
        else if (kind == RK_POOL && own < 0)
        {
            int w = (int)(mix(roll ^ 0x27d4eb2fu) % (uint32_t)kVoices);
            const Kit* kw = kit_[w].load(std::memory_order_acquire);
            if (kw && !kw->samples.empty()) { kk = kw; si = (int)(mix(roll ^ 0x165667b1u) % (uint32_t)kw->samples.size()); }
        }
        const Sample& s = kk->samples[(size_t)clamp(si, 0, (int)kk->samples.size() - 1)];
        if (!s.frames()) return;
        // the voice's other player fades out (a drum cuts itself); the closed hat cuts the open hat
        for (Player& q : players_[v]) if (q.active && q.fade < 0) q.fade = 64;
        if (v == 4 && getBool(P_CHOKE)) for (Player& q : players_[5]) if (q.active && q.fade < 0) q.fade = 96;
        Player* p = &players_[v][0];
        for (Player& q : players_[v]) if (!q.active) { p = &q; break; }
        if (p->active) p = &players_[v][1];
        float tune = (fvAt(v, step, F_TUNE) - 50.f) * 0.24f;
        if (kind == RK_PITCH) tune += -12.f + 24.f * frac(roll, 0x3c6ef372u); // up to an octave down or up
        p->s = &s;
        p->owner = kk;
        p->rev = fvAt(v, step, F_REVERSE) >= 50.f;
        if (kind == RK_FLIP) p->rev = !p->rev;
        p->rate = s.rate / sr_ * std::pow(2.0, tune / 12.0);
        p->end = (double)s.frames() - 1.0;
        p->pos = p->rev ? p->end - 1.0 : 0.0;
        float dec = fvAt(v, step, F_DECAY);
        p->decayLen = dec >= 99.5f ? 0.f : (float)(std::max(0.03f, dec / 100.f) * s.frames() / p->rate);
        if (kind == RK_SHORT) p->decayLen = (float)((0.06f + 0.22f * frac(roll, 0x9e3779b1u)) * s.frames() / p->rate);
        p->drive = fvAt(v, step, F_DRIVE) / 100.f;
        p->bits = bitsOf(fvAt(v, step, F_BITS));
        float fc = fvAt(v, step, F_FILTER);
        if (kind == RK_TEXTURE)
        {
            fc = std::min(fc, 30.f + 50.f * frac(roll, 0xc2b2ae35u));
            p->drive = std::max(p->drive, 0.2f + 0.5f * frac(roll, 0x7feb352du));
        }
        p->fa = fc >= 99.5f ? 1.f : 1.f - std::exp(-2.f * 3.14159265f * cutoffHz(fc) / (float)sr_);
        p->zl = p->zr = 0.f;
        float pan = (fvAt(v, step, F_PAN) - 50.f) / 50.f;
        p->panL = std::sqrt(0.5f * (1.f - pan)) * 1.4142f;
        p->panR = std::sqrt(0.5f * (1.f + pan)) * 1.4142f;
        if (kind == RK_GHOST) vel *= 0.3f;
        float lg = levelGain_[v];
        float lv = lockAt(v, step, LK_LEVEL); // STEPS: the step's own LEVEL
        if (!std::isnan(lv)) lg *= std::pow(10.f, (lv - get(P_LEVEL0 + v)) / 20.f);
        p->gain = 0.9f * (0.15f + 0.85f * vel) * lg;
        p->age = 0;
        p->fade = -1;
        p->active = true;
        if (step >= 0 && pk) swapped_[v].fetch_or(1ull << step, std::memory_order_relaxed);
    }

    bool sampleAt(Player& p, float& a, float& b) const
    {
        const Sample& s = *p.s;
        if (p.rev ? p.pos <= 0.0 : p.pos >= p.end) return false;
        size_t k = (size_t)p.pos;
        if (k + 1 >= s.frames()) return false;
        float f = (float)(p.pos - (double)k);
        a = (s.L[k] + (s.L[k + 1] - s.L[k]) * f) / 32768.f;
        if (s.R.empty()) b = a;
        else b = (s.R[k] + (s.R[k + 1] - s.R[k]) * f) / 32768.f;
        return true;
    }

    // ------------------------------------------------------------------ worker: folders
    static Sample toSample(const Wave& w, const std::string& name)
    {
        Sample s;
        s.rate = w.rate;
        s.name = name;
        s.L.resize(w.frames());
        for (size_t i = 0; i < w.frames(); ++i) s.L[i] = (int16_t)std::lround(std::max(-1.f, std::min(1.f, w.L[i])) * 32767.f);
        if (!w.R.empty())
        {
            s.R.resize(w.frames());
            for (size_t i = 0; i < w.frames(); ++i) s.R[i] = (int16_t)std::lround(std::max(-1.f, std::min(1.f, w.R[i])) * 32767.f);
        }
        return s;
    }

    // load a voice's folder (up to kMaxFiles WAVs, the chosen one always among them)
    bool loadVoice(int v, const std::string& folder, const std::string& file)
    {
        std::vector<std::string> files = listDir(folder, false);
        if (files.empty()) return false;
        int baseIdx = 0;
        for (size_t i = 0; i < files.size(); ++i) if (files[i] == file) baseIdx = (int)i;
        std::vector<int> take;
        if ((int)files.size() <= kMaxFiles) for (int i = 0; i < (int)files.size(); ++i) take.push_back(i);
        else
        {
            for (int i = 0; i < kMaxFiles; ++i) take.push_back((int)((long)i * (long)files.size() / kMaxFiles));
            if (std::find(take.begin(), take.end(), baseIdx) == take.end()) take[0] = baseIdx;
        }
        // up to kMaxFiles samples or kMaxVoiceBytes in memory, whichever comes first (the chosen one always)
        Kit* k = new Kit();
        size_t used = 0;
        for (int i : take)
        {
            if (i != baseIdx && used >= kMaxVoiceBytes) continue;
            Wave w;
            if (!loadWav(folder + "/" + files[(size_t)i], w, kMaxSeconds)) continue;
            if (i == baseIdx) k->base = (int)k->samples.size();
            k->samples.push_back(toSample(w, stripWav(files[(size_t)i])));
            k->files.push_back(files[(size_t)i]);
            used += (k->samples.back().L.size() + k->samples.back().R.size()) * 2;
        }
        if (k->samples.empty()) { delete k; return false; }
        installKit(v, k, {folder, stripWav(files[(size_t)baseIdx]), (int)k->samples.size()}, folder, files[(size_t)baseIdx]);
        return true;
    }

    // a voice's new kit goes to the audio thread; the texts and the state follow it
    void installKit(int v, Kit* k, const KitInfo& info, const std::string& folder, const std::string& file)
    {
        for (auto& r : retired_[v]) delete r.exchange(nullptr);
        delete pendingKit_[v].exchange(k, std::memory_order_acq_rel);
        size_t bytes = 0;
        for (const Sample& s : k->samples) bytes += (s.L.size() + s.R.size()) * 2;
        kitInfo_[v] = info;
        kitBytes_[v] = bytes;
        std::lock_guard<std::mutex> g(mtx_);
        voiceFolder_[v] = folder;
        voiceFile_[v] = file;
    }

    // the built-in sound (folder "")
    void builtInVoice(int v)
    {
        Kit* k = new Kit();
        k->samples.push_back(synth(v, (float)(sr_ > 0.0 ? sr_ : 44100.0)));
        installKit(v, k, {std::string(), "built in", 1}, std::string(), std::string());
    }

    // EMPTY: no sample, the voice stays silent (a short silence, so nothing else changes)
    void emptyVoice(int v)
    {
        Kit* k = new Kit();
        Sample s;
        s.rate = (float)(sr_ > 0.0 ? sr_ : 44100.0);
        s.name = "EMPTY";
        s.L.assign(64, 0);
        k->samples.push_back(s);
        installKit(v, k, {kEmptyFolder, "—", 0}, kEmptyFolder, std::string());
    }

    std::string folderLabel(const std::string& f) const
    {
        if (f.empty()) return "built in";
        if (f == kEmptyFolder) return "EMPTY";
        if (!root_.empty() && f.compare(0, root_.size(), root_) == 0 && f.size() > root_.size()) return f.substr(root_.size() + 1);
        return baseName(f);
    }

    // ------------------------------------------------------------------ worker: the ORDNER page
    void tapRow(int row)
    {
        std::string path = browser_.tap(row);
        if (path.empty()) { previewPath_.clear(); return; }
        if (hasExt(path, ".kit")) { previewPath_.clear(); loadKit(path); return; }
        int v = clamp(getInt(P_VOICE), 0, kVoices - 1);
        (void)v;
        if (path == previewPath_) loadMarked();
        else
        {
            previewPath_ = path;
            if (getBool(P_PREVIEW)) // 0.15: PREVIEW off = the first tap only marks the WAV
            {
                Wave w;
                if (!loadWav(path, w, 4.f)) { screen_.message(P_LISTINFO, "cannot read"); previewPath_.clear(); return; }
                Sample* s = new Sample(toSample(w, baseName(path)));
                for (auto& r : previewRetired_) delete r.exchange(nullptr);
                delete previewPending_.exchange(s, std::memory_order_acq_rel);
            }
            screen_.message(P_LISTINFO, "LOAD SAMPLE (or tap again)");
        }
    }

    // LOAD SAMPLE (or a 2nd tap): the marked WAV becomes the chosen voice's sample, its folder the voice's folder
    void loadMarked()
    {
        if (previewPath_.empty()) { screen_.message(P_LISTINFO, "tap a WAV first"); return; }
        std::string path = previewPath_;
        previewPath_.clear();
        int v = clamp(getInt(P_VOICE), 0, kVoices - 1);
        size_t i = path.find_last_of('/');
        if (loadVoice(v, path.substr(0, i), path.substr(i + 1))) screen_.message(P_LISTINFO, std::string(kVoiceNames[v]) + ": " + stripWav(baseName(path)));
        else screen_.message(P_LISTINFO, "cannot read");
    }

    // the two slots per voice are windows on all its functions: a slot that changes its function shows that
    // function's value; a knob that turns writes the value of the function it shows
    void syncSlots(bool force)
    {
        for (int i = 0; i < kVoices * kSlots; ++i)
        {
            int v = i / kSlots, f = clamp(getInt(P_FUNC0 + i), 0, NUM_FUNCS - 1);
            float val = get(P_VAL0 + i);
            if (force || f != shownFunc_[i])
            {
                shownFunc_[i] = f;
                shownVal_[i] = get(P_FV0 + v * NUM_FUNCS + f);
                if (std::fabs(val - shownVal_[i]) > 1e-4f) setParam(P_VAL0 + i, shownVal_[i]);
            }
            else if (std::fabs(val - shownVal_[i]) > 1e-4f)
            {
                shownVal_[i] = val;
                set(P_FV0 + v * NUM_FUNCS + f, val);
            }
        }
    }

    // ------------------------------------------------------------------ worker: SAVE SAMPLE, SAVE KIT, kits
    // where SAVE writes: the folder open on the FOLDER page, else the voice's folder, else the root folder
    std::string saveDir(int v)
    {
        std::string d = browser_.cwd();
        if (!d.empty() && d != "*" && isDir(d)) return d;
        {
            std::lock_guard<std::mutex> g(mtx_);
            if (!voiceFolder_[v].empty() && isDir(voiceFolder_[v])) return voiceFolder_[v];
        }
        return !root_.empty() && isDir(root_) ? root_ : std::string();
    }
    // the kits' own folder: <root folder>/Kits (made when needed); empty without a root folder
    std::string kitsDir()
    {
        if (root_.empty() || !isDir(root_)) return std::string();
        std::string d = root_ + "/Kits";
        if (!isDir(d)) ::mkdir(d.c_str(), 0777);
        return isDir(d) ? d : std::string();
    }
    static std::string freeName(const std::string& dir, const std::string& stem, const char* ext)
    {
        for (int n = 1; n < 1000; ++n)
        {
            char b[16];
            std::snprintf(b, sizeof b, " %02d", n);
            std::string f = stem + b + ext;
            if (FILE* x = std::fopen((dir + "/" + f).c_str(), "rb")) std::fclose(x);
            else return f;
        }
        return stem + ext;
    }
    static void put16(std::vector<uint8_t>& o, uint32_t v) { o.push_back((uint8_t)v); o.push_back((uint8_t)(v >> 8)); }
    static void put32(std::vector<uint8_t>& o, uint32_t v) { put16(o, v & 0xffff); put16(o, v >> 16); }

    // the voice's own sample through its functions (TUNE, REVERSE, DECAY, BITS, DRIVE, FILTER, PAN) as a 16-bit WAV
    void saveSample()
    {
        int v = clamp(getInt(P_VOICE), 0, kVoices - 1);
        Kit* k = kit_[v].load(std::memory_order_acquire); // kits are freed only by this thread
        if (!k || k->samples.empty()) { screen_.message(P_LISTINFO, "no sample"); return; }
        const Sample& s = k->samples[(size_t)clamp(k->base.load(), 0, (int)k->samples.size() - 1)];
        std::string dir = saveDir(v);
        if (dir.empty()) { screen_.message(P_LISTINFO, "open a folder first"); return; }
        Player p;
        p.s = &s;
        const double sr = s.rate;
        p.rev = funcValue(v, F_REVERSE, 0.f) >= 50.f;
        p.rate = std::pow(2.0, (funcValue(v, F_TUNE, 50.f) - 50.f) * 0.24 / 12.0);
        p.end = (double)s.frames() - 1.0;
        p.pos = p.rev ? p.end - 1.0 : 0.0;
        float dec = funcValue(v, F_DECAY, 100.f);
        p.decayLen = dec >= 99.5f ? 0.f : (float)(std::max(0.03f, dec / 100.f) * s.frames() / p.rate);
        p.drive = funcValue(v, F_DRIVE, 0.f) / 100.f;
        p.bits = bitsOf(funcValue(v, F_BITS, 100.f));
        float fc = funcValue(v, F_FILTER, 100.f);
        p.fa = fc >= 99.5f ? 1.f : 1.f - std::exp(-2.f * 3.14159265f * cutoffHz(fc) / (float)sr);
        float pan = (funcValue(v, F_PAN, 50.f) - 50.f) / 50.f;
        p.panL = std::min(1.f, std::sqrt(0.5f * (1.f - pan)) * 1.4142f);
        p.panR = std::min(1.f, std::sqrt(0.5f * (1.f + pan)) * 1.4142f);
        std::vector<float> L, Rr;
        float a, b;
        while (sampleAt(p, a, b) && L.size() < (size_t)(sr * 8.0))
        {
            float env = 1.f;
            if (p.decayLen > 0.f) { env = std::max(0.f, 1.f - p.age / p.decayLen); if (env <= 0.f) break; }
            if (p.bits < 16) { float q = (float)(1 << (p.bits - 1)); a = std::round(a * q) / q; b = std::round(b * q) / q; }
            if (p.drive > 0.01f) { float d = 1.f + 8.f * p.drive; a = std::tanh(a * d) / std::tanh(d); b = std::tanh(b * d) / std::tanh(d); }
            if (p.fa < 1.f) { p.zl += p.fa * (a - p.zl); p.zr += p.fa * (b - p.zr); a = p.zl; b = p.zr; }
            L.push_back(a * env * p.panL);
            Rr.push_back(b * env * p.panR);
            p.pos += p.rev ? -p.rate : p.rate;
            ++p.age;
        }
        if (L.empty()) { screen_.message(P_LISTINFO, "nothing to save"); return; }
        for (size_t i = 0; i < 64 && i < L.size(); ++i) { size_t j = L.size() - 1 - i; L[j] *= i / 64.f; Rr[j] *= i / 64.f; }
        const uint32_t n = (uint32_t)L.size(), rate = (uint32_t)std::lround(sr);
        std::vector<uint8_t> w;
        w.insert(w.end(), {'R', 'I', 'F', 'F'}); put32(w, 36 + n * 4);
        w.insert(w.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '}); put32(w, 16); put16(w, 1); put16(w, 2);
        put32(w, rate); put32(w, rate * 4); put16(w, 4); put16(w, 16);
        w.insert(w.end(), {'d', 'a', 't', 'a'}); put32(w, n * 4);
        for (uint32_t i = 0; i < n; ++i)
        {
            put16(w, (uint16_t)(int16_t)std::lround(std::max(-1.f, std::min(1.f, L[i])) * 32767.f));
            put16(w, (uint16_t)(int16_t)std::lround(std::max(-1.f, std::min(1.f, Rr[i])) * 32767.f));
        }
        std::string stem = stripWav(s.name);
        if (stem.size() > 20) stem.resize(20);
        std::string f = freeName(dir, stem, ".wav");
        if (writeFileBytes(dir + "/" + f, w.data(), w.size()))
        {
            if (browser_.cwd() == dir) browser_.list(dir + "/" + f);
            screen_.message(P_LISTINFO, "saved: " + stripWav(f), 4000);
        }
        else screen_.message(P_LISTINFO, "Error while writing");
    }

    // a kit: every voice's folder and sample, RANDOM, LEVEL and its four functions
    void saveKit()
    {
        std::string dir = kitsDir();
        const bool own = !dir.empty();
        if (!own) dir = saveDir(clamp(getInt(P_VOICE), 0, kVoices - 1)); // no root folder: where it was before
        if (dir.empty()) { screen_.message(P_LISTINFO, "set ROOT HERE for a Kits folder", 4000); return; }
        std::string t = "SGK2\n"; // per voice: folder, file, RANDOM, LEVEL, the 2 slots' functions, every function's value
        {
            std::lock_guard<std::mutex> g(mtx_);
            for (int v = 0; v < kVoices; ++v)
            {
                char b[64];
                t += voiceFolder_[v] + "\t" + voiceFile_[v];
                std::snprintf(b, sizeof b, "\t%.2f\t%.2f", get(P_RANDOM0 + v), get(P_LEVEL0 + v));
                t += b;
                for (int k = 0; k < kSlots; ++k)
                {
                    std::snprintf(b, sizeof b, "\t%d", getInt(P_FUNC0 + v * kSlots + k));
                    t += b;
                }
                for (int f = 0; f < NUM_FUNCS; ++f)
                {
                    std::snprintf(b, sizeof b, "\t%.2f", get(P_FV0 + v * NUM_FUNCS + f));
                    t += b;
                }
                t += "\n";
            }
        }
        std::string f = freeName(dir, "Kit", ".kit");
        if (writeFileBytes(dir + "/" + f, t.data(), t.size()))
        {
            if (browser_.cwd() == dir) browser_.list(dir + "/" + f);
            screen_.message(P_LISTINFO, (own ? "saved in Kits: " : "saved (no ROOT): ") + f.substr(0, f.size() - 4), 4000);
            kitName_ = f.substr(0, f.size() - 4) + "  ·  saved";
        }
        else screen_.message(P_LISTINFO, "Error while writing");
    }

    void setParam(int i, float x) { set(i, x); notifyHostParameterChanged(i); }

    void loadKit(const std::string& path)
    {
        std::string t;
        if (FILE* fp = std::fopen(path.c_str(), "rb"))
        {
            char buf[4096];
            size_t n;
            while ((n = std::fread(buf, 1, sizeof buf, fp)) > 0) t.append(buf, n);
            std::fclose(fp);
        }
        const bool v1 = t.compare(0, 5, "SGK1\n") == 0; // SCHLAG 0.3: 4 slots of function + value
        if (!v1 && t.compare(0, 5, "SGK2\n") != 0) { screen_.message(P_LISTINFO, "not a SCHLAG kit"); return; }
        size_t pos = 5;
        int missing = 0;
        for (int v = 0; v < kVoices && pos < t.size(); ++v)
        {
            size_t e = t.find('\n', pos);
            std::string line = t.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
            pos = e == std::string::npos ? t.size() : e + 1;
            std::vector<std::string> c;
            size_t a = 0;
            for (;;) { size_t b = line.find('\t', a); c.push_back(line.substr(a, b == std::string::npos ? std::string::npos : b - a)); if (b == std::string::npos) break; a = b + 1; }
            if (c.size() < (v1 ? 12u : (size_t)(4 + kSlots + NUM_FUNCS))) continue;
            if (!c[0].empty() && c[0] != kEmptyFolder && !isDir(c[0])) ++missing;
            {
                std::lock_guard<std::mutex> g(mtx_);
                voiceFolder_[v] = c[0];   // "" = built in, kEmptyFolder = EMPTY: the worker sets those too
                voiceFile_[v] = c[1];
                loadReq_[v] = true;
            }
            setParam(P_RANDOM0 + v, (float)std::atof(c[2].c_str()));
            setParam(P_LEVEL0 + v, (float)std::atof(c[3].c_str()));
            float fv[NUM_FUNCS];
            int slot[kSlots];
            for (int f = 0; f < NUM_FUNCS; ++f) fv[f] = defaultValue(f);
            if (v1)
                for (int k = 0; k < 4; ++k)
                {
                    int f = clamp(std::atoi(c[4 + 2 * k].c_str()), 0, NUM_FUNCS - 1);
                    fv[f] = (float)std::atof(c[5 + 2 * k].c_str());
                    if (k < kSlots) slot[k] = f;
                }
            else
            {
                for (int k = 0; k < kSlots; ++k) slot[k] = clamp(std::atoi(c[4 + k].c_str()), 0, NUM_FUNCS - 1);
                for (int f = 0; f < NUM_FUNCS; ++f) fv[f] = (float)std::atof(c[4 + kSlots + f].c_str());
            }
            for (int f = 0; f < NUM_FUNCS; ++f) setParam(P_FV0 + v * NUM_FUNCS + f, fv[f]);
            for (int k = 0; k < kSlots; ++k) setParam(P_FUNC0 + v * kSlots + k, (float)slot[k]);
        }
        syncSlots(true);
        std::string name = baseName(path);
        name = name.substr(0, name.size() - 4);
        loadName_ = missing ? name + ": " + std::to_string(missing) + " folders missing" : name + " loaded";
        kitName_ = name + (missing ? "  ·  " + std::to_string(missing) + " folders missing" : std::string("  ·  loaded"));
        screen_.message(P_LISTINFO, "KIT: loading " + name + " ...", 4000);
    }

    // < > on the main page: the voice's own sample steps through its folder (the samples in memory), at once
    void stepSample(int v, int dir)
    {
        Kit* k = kit_[v].load(std::memory_order_acquire); // kits are freed only by this thread
        if (!k || k->files.empty() || k->files[0].empty()) { screen_.message(P_HDRLINK, std::string(kVoiceNames[v]) + ": open a folder", 2500); return; }
        const int n = (int)k->samples.size();
        const int b = ((k->base.load() + dir) % n + n) % n;
        k->base.store(b);
        kitInfo_[v].file = k->samples[(size_t)b].name;
        {
            std::lock_guard<std::mutex> g(mtx_);
            voiceFile_[v] = k->files[(size_t)b];
        }
        char t[16];
        std::snprintf(t, sizeof t, " %d/%d", b + 1, n);
        screen_.message(P_HDRLINK, std::string(kVoiceNames[v]) + ": " + k->samples[(size_t)b].name.substr(0, 14) + t, 2500);
        audition(v, b);
    }

    // HIT ON / OFF (and DRAW): the step goes to GENRA CHOP's drum pattern (MIDI channel 10, PRINT, SCHLAG)
    bool canEdit()
    {
        if (getInt(P_AUTO) == 1) { screen_.message(P_STATUS, "TRACK: edit in the MPC grid", 3500); return false; }
        if (!link_.data() || !linkAlive_) { screen_.message(P_STATUS, "needs GENRA CHOP (LINK)", 3000); return false; }
        return true;
    }
    void toggleHit(int v, int st)
    {
        if (!canEdit()) return;
        const bool on = loop_.drum[v][st].vel == 0;
        editsOut_.push_back(v | st << 4 | (on ? 1 : 0) << 12);
        screen_.message(P_STATUS, on ? "HIT ON" : "HIT OFF", 1500);
    }
    // 0.15: COPY BAR / PASTE / CLEAR BAR: the shown bar's hits and every step's own values
    void copyBar()
    {
        const int b = clamp(getInt(P_BAR), 0, 3);
        for (int v = 0; v < kVoices; ++v)
            for (int j = 0; j < 16; ++j)
            {
                const int st = b * 16 + j;
                clip_.hit[v][j] = loop_.drum[v][st].vel;
                for (int i = 0; i < kLocks; ++i) clip_.lk[v][j][i] = lock_[v][st][i].load();
                clip_.smp[v][j] = smpLock_[v][st].load();
                clip_.cond[v][j] = condLock_[v][st].load();
            }
        clip_.has = true;
        screen_.message(P_STATUS, "COPY BAR " + std::to_string(b + 1) + ": now PASTE in another bar", 3000);
    }
    void pasteBar()
    {
        if (!clip_.has) { screen_.message(P_STATUS, "COPY BAR first", 2500); return; }
        const int b = clamp(getInt(P_BAR), 0, 3);
        const bool hits = getInt(P_AUTO) == 0 && link_.data() && linkAlive_;
        for (int v = 0; v < kVoices; ++v)
            for (int j = 0; j < 16; ++j)
            {
                const int st = b * 16 + j;
                for (int i = 0; i < kLocks; ++i) lock_[v][st][i].store(clip_.lk[v][j][i]);
                smpLock_[v][st].store(clip_.smp[v][j]);
                condLock_[v][st].store(clip_.cond[v][j]);
                const bool want = clip_.hit[v][j] > 0, has = loop_.drum[v][st].vel > 0;
                if (hits && want != has) editsOut_.push_back(v | st << 4 | (want ? 1 : 0) << 12);
            }
        syncLocks(true);
        screen_.message(P_STATUS, std::string("PASTE in BAR ") + std::to_string(b + 1) + (hits ? "" : " (values only)"), 3000);
    }
    void clearBar()
    {
        const int b = clamp(getInt(P_BAR), 0, 3);
        const bool hits = getInt(P_AUTO) == 0 && link_.data() && linkAlive_;
        for (int v = 0; v < kVoices; ++v)
            for (int j = 0; j < 16; ++j)
            {
                const int st = b * 16 + j;
                for (int i = 0; i < kLocks; ++i) lock_[v][st][i].store(std::numeric_limits<float>::quiet_NaN());
                smpLock_[v][st].store(-1);
                condLock_[v][st].store(0);
                if (hits && loop_.drum[v][st].vel > 0) editsOut_.push_back(v | st << 4);
            }
        syncLocks(true);
        screen_.message(P_STATUS, std::string("CLEAR BAR ") + std::to_string(b + 1) + (hits ? ": UNDO in GENRA CHOP" : " (values)"), 3000);
    }
    // 0.15: FOLDER: the last three parts of the open folder's path (tap = go there)
    std::string segPath(int i) const
    {
        const std::string& c = browser_.cwd();
        if (c.empty() || c == "*") return std::string();
        std::vector<size_t> ends;
        for (size_t k = 1; k <= c.size(); ++k) if (k == c.size() || c[k] == '/') ends.push_back(k);
        const int n = (int)ends.size(), first = std::max(0, n - kSegs), at = first + i;
        if (at >= n) return std::string();
        return c.substr(0, ends[(size_t)at]);
    }
    std::string segText(int i) const
    {
        const std::string d = segPath(i);
        if (d.empty()) return i == 0 && browser_.cwd().empty() ? std::string("Drives") : std::string();
        return baseName(d);
    }

    // 0.15: a sample plays once when chosen, if PREVIEW is on
    void audition(int v, int sample) { if (getBool(P_PREVIEW)) audition_.store(v * 1024 + sample + 1); }

    // LOAD KIT: the list shows the Kits folder
    void openKits()
    {
        std::string d = kitsDir();
        if (d.empty()) { screen_.message(P_LISTINFO, "set ROOT HERE for a Kits folder", 4000); return; }
        browser_.open(d);
        previewPath_.clear();
        screen_.message(P_LISTINFO, "Kits: tap a kit to load it", 3000);
    }

    // ------------------------------------------------------------------ worker: screen
    long long nowMs() const
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    void texts(genralink::LinkData* L, bool alive)
    {
        const char* kn[12] = {"C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};
        int kr = L ? L->keyRoot.load() : -1;
        std::string key = kr >= 0 && kr < 12 ? std::string(kn[kr]) + (L->keyMinor.load() ? "m" : "") : std::string("-");
        int bpm100 = L ? L->bpmX100.load() : 0;
        char b[64];
        if (alive) std::snprintf(b, sizeof b, "LINK GENRA CHOP · %.0f BPM", bpm100 / 100.0);
        else std::snprintf(b, sizeof b, "no GENRA CHOP");
        screen_.setText(P_HDRLINK, b);
        {
            const int qv = clamp(getInt(P_VOICE), 0, kVoices - 1);
            screen_.setText(P_QLTXT, std::string("Q: ") + kVoiceNames[qv]);
            for (int v = 0; v < kVoices; ++v)
            {
                screen_.setText(P_VNAME0 + v, v == qv ? std::string() : std::string(kVoiceNames[v]));
                screen_.setText(P_VNAMESEL0 + v, v == qv ? std::string(kVoiceNames[v]) : std::string());
                screen_.setFrame(P_VSEL0 + v, v == qv ? 1 : 0);
            }
        }
        for (int v = 0; v < kVoices; ++v)
        {
            screen_.setText(P_FOLDERTXT0 + v, folderLabel(kitInfo_[v].folder));
            screen_.setText(P_SAMPLETXT0 + v, kitInfo_[v].file);
        }
        int sv = clamp(getInt(P_VOICE), 0, kVoices - 1);
        screen_.setText(P_VOICEINFO, kVoiceNames[sv]);
        screen_.setText(P_VFOLDER, "Folder: " + folderLabel(kitInfo_[sv].folder));
        screen_.setText(P_VSAMPLE, kitInfo_[sv].file);
        screen_.setText(P_VCOUNT, std::to_string(kitInfo_[sv].count) + " samples for RANDOM (max 128)");
        screen_.setText(P_KITTXT, kitName_.empty() ? std::string("—") : kitName_);
        screen_.setText(P_PAGETXT, browser_.pageText());
        for (int i = 0; i < kSegs; ++i) screen_.setText(P_SEGTXT0 + i, segText(i));
        for (int v = 0; v < kVoices; ++v)
        {
            // 0.15: LEVEL as a bar; "+n": functions off neutral that neither slot shows
            const float lv = get(P_LEVEL0 + v);
            screen_.setFrame(P_LVLBAR0 + v, clamp((int)std::lround((lv + 40.f) / 46.f * (kLvlFrames - 1)), 0, kLvlFrames - 1));
            int more = 0;
            for (int g = 0; g < NUM_FUNCS; ++g)
            {
                bool shown = false;
                for (int k = 0; k < kSlots; ++k) shown = shown || getInt(P_FUNC0 + v * kSlots + k) == g;
                if (!shown && std::fabs(get(P_FV0 + v * NUM_FUNCS + g) - defaultValue(g)) > 0.5f) ++more;
            }
            screen_.setText(P_MORETXT0 + v, more ? "+" + std::to_string(more) : std::string());
        }
        screen_.setText(P_PATH, browser_.pathText(), true);
        screen_.setText(P_LISTINFO, browser_.info());
        std::string loaded;
        {
            std::lock_guard<std::mutex> g(mtx_);
            loaded = voiceFolder_[sv].empty() ? std::string() : voiceFolder_[sv] + "/" + voiceFile_[sv];
        }
        for (int r = 0; r < kRows; ++r)
        {
            screen_.setText(P_ROWT0 + r, browser_.rowText(r, loaded, previewPath_));
            screen_.setFrame(P_ROWLIT0 + r, browser_.isSelected(r, previewPath_) ? 2 : browser_.isSelected(r, loaded) ? 1 : 0);
        }
        std::snprintf(b, sizeof b, "Drums · 4 bars · %.0f BPM · %s", bpm100 / 100.0, key.c_str());
        screen_.setText(P_PRINTINFO, b);
        screen_.setText(P_PRINTSTATUS, getInt(P_AUTO) == 0 ? "GENRA: GENRA's loop plays" : "TRACK: only the track's notes");
        screen_.setText(P_LEARNINFO, pads_.learning() ? pads_.info() : (pads_.learned() ? "learned" : "not learned yet"));
        screen_.setText(P_MAPINFO, pads_.info());
        screen_.setText(P_ROOTTXT, root_.empty() ? std::string("none yet (FOLDER: ROOT HERE)") : pathLabel(root_), true);
        screen_.setText(P_LINKINFO, !L ? "Error: " + link_.error() : (alive ? std::string("GENRA CHOP linked") : std::string("no GENRA CHOP")));
        size_t bytes = 0;
        for (size_t x : kitBytes_) bytes += x;
        std::snprintf(b, sizeof b, "%.1f MB samples loaded", bytes / 1048576.0);
        screen_.setText(P_MEMINFO, b);
        // DIAG's state (texts on the MPC: at most 31 characters)
        if (diag_.running()) std::snprintf(b, sizeof b, "running %d s · %ld events", diag_.secondsLeft(), diag_.events());
        else if (!diag_.error().empty()) std::snprintf(b, sizeof b, "%.30s", diag_.error().c_str());
        else if (!diag_.lastFile().empty())
        {
            const std::string f = diag_.lastFile();
            const size_t k = f.rfind("/logs/");
            std::snprintf(b, sizeof b, "saved Genra%.24s", k == std::string::npos ? f.c_str() : f.c_str() + k);
        }
        else std::snprintf(b, sizeof b, "tap DIAG: 2 min capture");
        screen_.setText(P_DIAGINFO, b);
        int step = curStep_.load();
        screen_.setText(P_STATUS, step >= 0 ? "Bar " + std::to_string(step / 16 + 1) + " / 4" : std::string(getInt(P_AUTO) == 0 ? "GENRA" : "TRACK"));
    }

    // ------------------------------------------------------------------ worker: STEPS
    bool hasLock(int v, int st) const
    {
        for (int i = 0; i < kLocks; ++i) if (!std::isnan(lock_[v][st][i].load())) return true;
        return condLock_[v][st].load() > 0;
    }
    // a lane's (the main page's) value behind editor knob i, and where it is written
    float laneValue(int v, int i) const
    {
        if (i == 0) return get(P_RANDOM0 + v);
        if (i == LK_LEVEL) return get(P_LEVEL0 + v);
        if (i == LK_CHANCE) return 100.f;
        return funcValue(v, i - 1, 0.f);
    }
    void setLaneValue(int v, int i, float x)
    {
        if (i == 0) setParam(P_RANDOM0 + v, x);
        else if (i == LK_LEVEL) setParam(P_LEVEL0 + v, x);
        else if (i != LK_CHANCE) { setParam(P_FV0 + v * NUM_FUNCS + i - 1, x); syncSlots(true); }
    }
    // the editor's knobs: a step (its own values, else the main page's; a knob turned = the step's own) or a whole
    // lane (the main page's values, written there)
    // Q-LINKS: FUNCTIONS: the stand-ins follow the chosen voice; turning one writes the voice's value, a value changed
    // elsewhere (main page, STEPS' lane, a kit) comes back to the stand-in
    // 0.12: from top to bottom like the column: SAMPLE (steps through the folder), RANDOM, the upper and the lower
    // function slot (the function chosen there), LEVEL
    int qlTarget(int v, int i) const
    {
        switch (i)
        {
            case QL_RANDOM: return P_RANDOM0 + v;
            case QL_UPPER: return P_VAL0 + v * kSlots;
            case QL_LOWER: return P_VAL0 + v * kSlots + 1;
            case QL_LEVEL: return P_LEVEL0 + v;
            case QL_KIND: return P_RKIND0 + v;
            default: return -1; // SAMPLE: the kit's sample (no parameter)
        }
    }
    // a knob 0..100 over n samples (and back)
    static int sampleOfKnob(float x, int n) { return n <= 1 ? 0 : clamp((int)std::lround(x / 100.f * (float)(n - 1)), 0, n - 1); }
    static float knobOfSample(int i, int n) { return n <= 1 ? 0.f : 100.f * (float)i / (float)(n - 1); }
    float qlValue(int v, int i) const
    {
        if (i != QL_SAMPLE) return get(qlTarget(v, i));
        Kit* k = kit_[v].load(std::memory_order_acquire);
        const int n = k ? (int)k->samples.size() : 0;
        return n > 0 ? knobOfSample(clamp(k->base.load(), 0, n - 1), n) : 0.f;
    }
    // the voice's own sample: index i of its folder (like < > on the main page)
    void setSample(int v, int i)
    {
        Kit* k = kit_[v].load(std::memory_order_acquire);
        if (!k || k->samples.empty()) return;
        const int n = (int)k->samples.size(), cur = clamp(k->base.load(), 0, n - 1);
        if (i != cur) stepSample(v, i - cur);
    }
    void syncQ(bool force)
    {
        const int v = clamp(getInt(P_VOICE), 0, kVoices - 1);
        if (force || v != qlVoice_)
        {
            qlVoice_ = v;
            for (int i = 0; i < kQL; ++i) { qlShown_[i] = qlValue(v, i); setParam(P_QL0 + i, qlShown_[i]); }
            return;
        }
        bool slots = false;
        for (int i = 0; i < kQL; ++i)
        {
            const float pv = get(P_QL0 + i), tv = qlValue(v, i);
            if (std::fabs(pv - qlShown_[i]) > 1e-4f)
            {
                qlShown_[i] = pv;
                if (i == QL_SAMPLE)
                {
                    Kit* k = kit_[v].load(std::memory_order_acquire);
                    const int n = k ? (int)k->samples.size() : 0;
                    if (n > 0) setSample(v, sampleOfKnob(pv, n));
                    else screen_.message(P_HDRLINK, std::string(kVoiceNames[v]) + ": open a folder", 2500);
                }
                else { setParam(qlTarget(v, i), pv); slots = slots || i == QL_UPPER || i == QL_LOWER; }
            }
            else if (i == QL_SAMPLE)
            {
                // the stand-in stays where it was turned while it still points at the voice's sample
                Kit* k = kit_[v].load(std::memory_order_acquire);
                const int n = k ? (int)k->samples.size() : 0;
                if (n > 0 && sampleOfKnob(qlShown_[i], n) != sampleOfKnob(tv, n)) { qlShown_[i] = tv; setParam(P_QL0 + i, tv); }
            }
            else if (std::fabs(tv - qlShown_[i]) > 1e-4f) { qlShown_[i] = tv; setParam(P_QL0 + i, tv); }
        }
        if (slots) syncSlots(false); // the slot's knob moved: its function's value follows
    }

    // ------------------------------------------------------------------ STEPS: the two knobs (0.12)
    // a knob shows / sets one thing of the selected step (or of the whole lane): SAMPLE, CONDITION or a lock
    static int lockOfFn(int f)
    {
        switch (f)
        {
            case SF_LEVEL: return LK_LEVEL;
            case SF_CHANCE: return LK_CHANCE;
            case SF_RANDOM: return 0;
            case SF_SAMPLE: case SF_COND: return -1;
            default: return 1 + (f - SF_TUNE); // TUNE .. FILTER = the functions 1..8
        }
    }
    static float lockToKnob(int i, float x) { return i == LK_LEVEL ? (x + 40.f) / 46.f * 100.f : x; }
    static float knobToLock(int i, float k) { return i == LK_LEVEL ? -40.f + k * 0.46f : k; }
    int stepVoice() const { return stepSel_ >= 0 ? stepSel_ / kSteps : laneSel_; }
    // where the knob of function f stands now (0..100)
    float stepKnobTarget(int f) const
    {
        const int v = stepVoice();
        if (f == SF_SAMPLE)
        {
            Kit* k = v >= 0 ? kit_[v].load(std::memory_order_acquire) : nullptr;
            const int n = k ? (int)k->samples.size() : 0;
            if (n <= 0) return 0.f;
            if (stepSel_ >= 0) return 100.f * (float)(smpLock_[v][stepSel_ % kSteps].load() + 1) / (float)n; // -1 .. n-1
            return knobOfSample(clamp(k->base.load(), 0, n - 1), n);
        }
        if (f == SF_COND) return stepSel_ >= 0 ? 100.f * (float)condLock_[v][stepSel_ % kSteps].load() / (float)(NUM_CONDS - 1) : 0.f;
        const int i = lockOfFn(f);
        return lockToKnob(i, get(P_SLK0 + i));
    }
    void stepKnobTurned(int f, float x)
    {
        const int v = stepVoice();
        if (v < 0) { screen_.message(P_STATUS, "tap a step first", 2500); return; }
        if (f == SF_SAMPLE)
        {
            Kit* k = kit_[v].load(std::memory_order_acquire);
            const int n = k ? (int)k->samples.size() : 0;
            if (n <= 0) { screen_.message(P_STATUS, std::string(kVoiceNames[v]) + ": open a folder", 2500); return; }
            if (stepSel_ < 0) { setSample(v, sampleOfKnob(x, n)); return; }
            const int st = stepSel_ % kSteps, i = clamp((int)std::lround(x / 100.f * (float)n) - 1, -1, n - 1);
            if (i != smpLock_[v][st].load())
            {
                smpLock_[v][st].store((int16_t)i);
                audition(v, i >= 0 ? i : k->base.load());
            }
            return;
        }
        if (f == SF_COND)
        {
            if (stepSel_ < 0) { screen_.message(P_STATUS, "CONDITION: tap a step", 2500); return; }
            condLock_[v][stepSel_ % kSteps].store((int8_t)clamp((int)std::lround(x / 100.f * (NUM_CONDS - 1)), 0, NUM_CONDS - 1));
            return;
        }
        const int i = lockOfFn(f);
        setParam(P_SLK0 + i, knobToLock(i, x)); // syncLocks writes it to the step (or the lane)
    }
    void syncStepKnobs()
    {
        const int sel = stepSel_ >= 0 ? stepSel_ : laneSel_ >= 0 ? 100000 + laneSel_ : -1;
        const bool force = sel != skSel_;
        skSel_ = sel;
        for (int k = 0; k < kStepKnobs; ++k)
        {
            const int f = clamp(getInt(P_SKF0 + k), 0, NUM_SF - 1);
            const float cur = get(P_SKN0 + k), tv = stepKnobTarget(f);
            if (force || f != skFn_[k]) { skFn_[k] = f; skShown_[k] = tv; if (std::fabs(cur - tv) > 1e-4f) setParam(P_SKN0 + k, tv); }
            else if (std::fabs(cur - skShown_[k]) > 1e-4f) { skShown_[k] = cur; stepKnobTurned(f, cur); }
            else if (std::fabs(tv - skShown_[k]) > 0.5f) { skShown_[k] = tv; setParam(P_SKN0 + k, tv); }
        }
    }
    std::string stepKnobText(int k) const
    {
        const int f = clamp(getInt(P_SKF0 + k), 0, NUM_SF - 1), v = stepVoice();
        if (v < 0) return "—";
        const int st = stepSel_ >= 0 ? stepSel_ % kSteps : -1;
        if (f == SF_SAMPLE)
        {
            Kit* k2 = kit_[v].load(std::memory_order_acquire);
            if (!k2 || k2->samples.empty()) return "no folder";
            const int n = (int)k2->samples.size();
            const int i = st >= 0 ? smpLock_[v][st].load() : clamp(k2->base.load(), 0, n - 1);
            return i >= 0 && i < n ? k2->samples[(size_t)i].name.substr(0, 24) : std::string("— voice's sample");
        }
        if (f == SF_COND) return st >= 0 ? kCondNames[clamp((int)condLock_[v][st].load(), 0, NUM_CONDS - 1)] : "—";
        const int i = lockOfFn(f);
        if (st >= 0 && std::isnan(lock_[v][st][i].load())) return "— " + (i == LK_CHANCE ? std::string("100 %") : lockText(i, laneValue(v, i)));
        return lockText(i, get(P_SLK0 + i));
    }

    void syncLocks(bool force)
    {
        const int v = stepSel_ >= 0 ? stepSel_ / kSteps : laneSel_, st = stepSel_ >= 0 ? stepSel_ % kSteps : 0;
        for (int i = 0; i < kLocks; ++i)
        {
            float val = get(P_SLK0 + i);
            if (force)
            {
                float x = stepSel_ >= 0 ? lock_[v][st][i].load() : std::numeric_limits<float>::quiet_NaN();
                if (std::isnan(x)) x = v < 0 ? lockDefault(i) : laneValue(v, i);
                shownLk_[i] = x;
                if (std::fabs(val - x) > 1e-4f) setParam(P_SLK0 + i, x);
            }
            else if (std::fabs(val - shownLk_[i]) > 1e-4f)
            {
                shownLk_[i] = val;
                if (stepSel_ >= 0) lock_[v][st][i].store(val);
                else if (laneSel_ >= 0) setLaneValue(laneSel_, i, val);
            }
        }
    }
    void stepTexts()
    {
        static const char* bars[4] = {"1", "2", "3", "4"};
        const int bar = clamp(getInt(P_BAR), 0, 3);
        for (int k = 0; k < kStepKnobs; ++k)
        {
            screen_.setText(P_SKFTXT0 + k, kStepFnNames[clamp(getInt(P_SKF0 + k), 0, NUM_SF - 1)]);
            screen_.setText(P_SKVTXT0 + k, stepKnobText(k));
        }
        if (stepSel_ >= 0)
        {
            int v = stepSel_ / kSteps, st = stepSel_ % kSteps;
            screen_.setText(P_STEPHDR, std::string(kVoiceNames[v]) + " · BAR " + bars[st / 16] + " · STEP " + std::to_string(st % 16 + 1));
            for (int i = 0; i < kLocks; ++i)
            {
                float x = lock_[v][st][i].load();
                screen_.setText(P_SLKTXT0 + i, std::isnan(x) ? std::string("—") : lockText(i, x));
            }
            int sm = smpLock_[v][st].load();
            Kit* k = kit_[v].load();
            screen_.setText(P_SMPTXT, sm >= 0 && k && sm < (int)k->samples.size() ? k->samples[(size_t)sm].name : std::string("—"));
            screen_.setText(P_CONDTXT, kCondNames[clamp((int)condLock_[v][st].load(), 0, NUM_CONDS - 1)]);
        }
        else if (laneSel_ >= 0)
        {
            int v = laneSel_;
            screen_.setText(P_STEPHDR, std::string(kVoiceNames[v]) + " · WHOLE LANE");
            for (int i = 0; i < kLocks; ++i) screen_.setText(P_SLKTXT0 + i, i == LK_CHANCE ? std::string("—") : lockText(i, laneValue(v, i)));
            Kit* k = kit_[v].load();
            screen_.setText(P_SMPTXT, k && !k->samples.empty() ? k->samples[(size_t)clamp(k->base.load(), 0, (int)k->samples.size() - 1)].name : std::string("—"));
            screen_.setText(P_CONDTXT, "—");
        }
        else
        {
            screen_.setText(P_STEPHDR, getInt(P_DRAW) == 0 ? "DRAW: tap = hit on / off" : "EDIT: tap a step");
            for (int i = 0; i < kLocks; ++i) screen_.setText(P_SLKTXT0 + i, "—");
            screen_.setText(P_SMPTXT, "—");
            screen_.setText(P_CONDTXT, "—");
        }
        int step = curStep_.load();
        screen_.setText(P_STEPPOS, step >= 0 ? std::string(bars[(step / 16) & 3]) + " · " + std::to_string(step % 16 + 1) : std::string("STOP"));
        for (int v = 0; v < kVoices; ++v)
        {
            char b[16];
            std::snprintf(b, sizeof b, "%.0f", get(P_RANDOM0 + v));
            screen_.setText(P_RINGTXT0 + v, b);
        }
        // the grid: frame = (((base * 3 + nudge) * 3 + dot) * 2 + own sample) * 2 + selected
        const genralink::Loop& l = loop_;
        const bool autoMode = getInt(P_AUTO) == 0;
        for (int v = 0; v < kVoices; ++v)
        {
            const uint64_t seen = seenMask_[v].load();
            for (int j = 0; j < 16; ++j)
            {
                int st = bar * 16 + j;
                const genralink::DrumHit& h = l.drum[v][st];
                int vel = autoMode && h.vel ? h.vel : (seen >> st & 1u) ? 127 : 0;
                int base = vel ? (vel >= 76 ? 3 : 2) : ((j / 4) & 1);
                int nudge = !(autoMode && h.vel) ? 0 : h.off < -8 ? 1 : h.off > 8 ? 2 : 0;
                float own = lock_[v][st][0].load();
                int dot = !std::isnan(own) && own > 0.f ? 2 : hasLock(v, st) ? 1 : 0;
                int smp = smpLock_[v][st].load() >= 0;
                int sel = stepSel_ == v * kSteps + st;
                screen_.setFrame(P_SCELL0 + v * 16 + j, (((base * 3 + nudge) * 3 + dot) * 2 + smp) * 2 + sel);
            }
        }
        for (int j = 0; j < 16; ++j) screen_.setFrame(P_PLAY0 + j, step >= 0 && step / 16 == bar && step % 16 == j);
        // 0.15: the chips: the step's own values (tap = that function on knob 1); dots: bars with hits
        int nc = 0;
        if (stepSel_ >= 0)
        {
            const int v = stepSel_ / kSteps, st = stepSel_ % kSteps;
            for (int f = 0; f < NUM_SF && nc < kChips; ++f)
            {
                bool own = f == SF_SAMPLE ? smpLock_[v][st].load() >= 0
                         : f == SF_COND ? condLock_[v][st].load() > 0
                         : !std::isnan(lock_[v][st][lockOfFn(f)].load());
                if (own) chipFn_[nc++] = f;
            }
        }
        for (int i = 0; i < kChips; ++i)
        {
            if (i >= nc) chipFn_[i] = -1;
            screen_.setText(P_CHIPTXT0 + i, i < nc ? std::string(kStepFnNames[chipFn_[i]]) : std::string());
            screen_.setFrame(P_CHIPLIT0 + i, i < nc ? 1 : 0);
        }
        for (int b = 0; b < 4; ++b)
        {
            bool any = false;
            for (int v = 0; v < kVoices && !any; ++v)
                for (int j = 0; j < 16 && !any; ++j)
                    any = (autoMode && l.drum[v][b * 16 + j].vel) || (seenMask_[v].load() >> (b * 16 + j) & 1u);
            screen_.setText(P_BARDOT0 + b, any ? "• • •" : "");
        }
    }

    void cells()
    {
        const genralink::Loop& l = loop_;
        int step = curStep_.load();
        int bar = step >= 0 ? step / 16 : 0;
        uint32_t lit = litMask_.load();
        for (int v = 0; v < kVoices; ++v)
        {
            uint64_t sw = swapped_[v].load();
            const bool autoMode = getInt(P_AUTO) == 0;
            uint64_t loopHits = 0;
            if (autoMode) for (int st = 0; st < kSteps; ++st) if (l.drum[v][st].vel) loopHits |= 1ull << st;
            const uint64_t mask = loopHits | seenMask_[v].load();
            for (int j = 0; j < 16; ++j)
            {
                int s = bar * 16 + j;
                bool hit = (mask >> s) & 1u;
                bool rnd = hit && (sw >> s & 1u);
                // HOLD, or the loop not playing: which hits swap (the same roll the audio thread uses)
                if (hit && (step < 0 || !liveStable_[v].load()))
                {
                    uint32_t roll;
                    rnd = picks(v, s, lastRound_[v].load(), mask, roll);
                }
                screen_.setFrame(P_CELL0 + v * 16 + j, (hit ? (rnd ? 2 : 1) : 0) + (step >= 0 && step % 16 == j ? 3 : 0));
            }
            screen_.setFrame(P_LAMP0 + v, (lit >> v) & 1u);
            screen_.setFrame(P_PADLIT0 + v, (lit >> v) & 1u);
        }
    }

    void print(genralink::LinkData* L)
    {
        std::string dir = printDir(std::string());
        if (dir.empty()) { screen_.message(P_PRINTSTATUS, "no folder to write to"); return; }
        const genralink::Loop& l = loop_;
        const float swing = l.swingX1000 / 1000.f;
        bool solo = false;
        for (int v = 0; v < kVoices; ++v) if (getBool(P_SOLO0 + v)) solo = true;
        std::vector<MidiNote> notes;
        for (int v = 0; v < kVoices; ++v)
        {
            if (getBool(P_MUTE0 + v) || (solo && !getBool(P_SOLO0 + v))) continue;
            for (int s = 0; s < kSteps; ++s)
            {
                int rep = repeatOf(v, s);
                const genralink::DrumHit& h = l.drum[v][s];
                if (!h.vel || h.chance < 128) continue;
                double at = s + ((s & 1) ? (swing - 0.5f) * 2.f : 0.f) + std::max(0, (int)h.off) / 100.0;
                int r = std::max(std::max(1, (int)h.ratchet), rep);
                for (int k = 0; k < r; ++k)
                    notes.push_back({at + (double)k / r, 0.25 / r, pads_.noteOf(v), (int)(h.vel * (k ? 0.75f : 1.f))});
            }
        }
        int bpm100 = L ? L->bpmX100.load() : 0;
        double bpm = bpm100 > 0 ? bpm100 / 100.0 : 120.0;
        int no = printNumberFor(dir, l.id);
        char name[96];
        std::snprintf(name, sizeof name, "Print %02d Drums %.0f", no, bpm);
        std::vector<uint8_t> f = midiFile(notes, bpm, name, kSteps);
        if (writeFileBytes(dir + "/" + name + ".mid", f.data(), f.size()))
        {
            set(P_AUTO, 1.f);
            notifyHostParameterChanged(P_AUTO);
            screen_.message(P_PRINTSTATUS, std::string(name) + " saved, now TRACK", 4000);
        }
        else screen_.message(P_PRINTSTATUS, "Error while writing");
        printList();
    }

    void printList()
    {
        std::string dir = printDir(std::string());
        screen_.setText(P_PRINTDIRTXT, dir.empty() ? std::string("no folder") : pathLabel(dir), true);
        std::vector<std::string> list = dir.empty() ? std::vector<std::string>() : listPrints(dir);
        for (int r = 0; r < kPrintRows; ++r)
            screen_.setText(P_PRINTROW0 + r, r < (int)list.size() ? (r == 0 ? "> " : "") + list[(size_t)r].substr(0, list[(size_t)r].size() - 4) : std::string());
    }

    void workerMain()
    {
        root_ = settings_.get("root");
        browser_.rows = kRows;
        browser_.extra = ".kit";
        browser_.open(!root_.empty() ? root_ : settings_.get("lastdir"));
        uint32_t lastSeq = 1;
        genralink::Loop tmp;
        genralink::Link::clearLoop(loop_);
        genralink::Link::Alive alive;
        int lastVoice = -1;
        // RANDOM and HOLD | LIVE count only when steady for 40 ms (the MPC probes values for a moment)
        float lastRandom[kVoices], pendRandom[kVoices];
        long long pendRandomAt[kVoices], pendLiveAt[kVoices];
        bool pendLive[kVoices];
        for (int v = 0; v < kVoices; ++v)
        {
            lastRandom[v] = pendRandom[v] = get(P_RANDOM0 + v);
            pendRandomAt[v] = pendLiveAt[v] = 0;
            pendLive[v] = liveStable_[v].load();
        }
        long long lastSlow = 0;
        while (!quit_.load())
        {
            genralink::LinkData* L = link_.data();
            if (resyncReq_.exchange(false)) screen_.resync();
            // kits and projects: one voice per look, so the screen shows how far it is ("KIT: loading 3/8")
            {
                int pending = 0, v = -1;
                std::string folder, file;
                {
                    std::lock_guard<std::mutex> g(mtx_);
                    for (int i = 0; i < kVoices; ++i) if (loadReq_[i]) { if (v < 0) v = i; ++pending; }
                    if (v >= 0) { loadReq_[v] = false; folder = voiceFolder_[v]; file = voiceFile_[v]; }
                }
                if (v >= 0)
                {
                    if (!loadTotal_) loadTotal_ = pending;
                    char b[64];
                    std::snprintf(b, sizeof b, "KIT: loading %d/%d · %s", loadTotal_ - pending + 1, loadTotal_, kVoiceNames[v]);
                    screen_.message(P_STATUS, b, 3000);
                    screen_.message(P_LISTINFO, b, 3000);
                    texts(L, linkAlive_);
                    screen_.push();
                    if (folder == kEmptyFolder) emptyVoice(v);
                    else if (folder.empty()) builtInVoice(v);
                    else if (!loadVoice(v, folder, file)) { screen_.message(P_STATUS, std::string(kVoiceNames[v]) + ": folder missing", 4000); ++loadMissing_; }
                    if (pending == 1)
                    {
                        std::string done = !loadName_.empty() ? loadName_ : loadMissing_ ? std::to_string(loadMissing_) + " folders missing" : "samples loaded";
                        screen_.message(P_STATUS, "KIT: " + done, 4000);
                        screen_.message(P_LISTINFO, "KIT: " + done, 4000);
                        loadTotal_ = 0; loadMissing_ = 0; loadName_.clear();
                    }
                }
            }
            bool on = L && alive.check(L->writerBeat.load(), nowMs());
            linkAlive_ = on;
            if (L)
            {
                uint32_t seq = 0;
                if (L->magic.load() == genralink::kMagic && L->loopSeq.load() != lastSeq && link_.readLoop(tmp, seq) && seq != 0)
                {
                    lastSeq = seq;
                    loop_ = tmp;
                    int r = ready_.load(), u = inUse_.load(), w = 0;
                    while (w == r || w == u) ++w;
                    loops_[w] = tmp;
                    ready_.store(w, std::memory_order_release);
                    if (L->loopEditOf.load() == seq) readyNow_.store(true); // a hand edit: play it at once
                    for (auto& s : swapped_) s.store(0);
                }
            }
            // RANDOM: every turn rolls again (HOLD keeps that roll); LIVE -> HOLD keeps the last pass
            const long long now = nowMs();
            if (randomSync_.exchange(false))
                for (int v = 0; v < kVoices; ++v) { lastRandom[v] = pendRandom[v] = get(P_RANDOM0 + v); pendRandomAt[v] = 0; }
            for (int v = 0; v < kVoices; ++v)
            {
                float r = get(P_RANDOM0 + v);
                if (std::fabs(r - lastRandom[v]) <= 0.4f) { pendRandom[v] = lastRandom[v]; pendRandomAt[v] = 0; }
                else if (std::fabs(r - pendRandom[v]) > 0.01f || !pendRandomAt[v]) { pendRandom[v] = r; pendRandomAt[v] = now; }
                else if (now - pendRandomAt[v] >= 40)
                {
                    lastRandom[v] = r;
                    pendRandomAt[v] = 0;
                    seed_[v].fetch_add(1);
                    swapped_[v].store(0);
                    Kit* k = kit_[v].load();
                    if (r > 0.5f && k && k->samples.size() <= 1) screen_.message(P_STATUS, std::string(kVoiceNames[v]) + ": RANDOM: 1 sample in folder");
                }
                bool live = getBool(P_LIVE0 + v);
                if (live == liveStable_[v].load()) { pendLive[v] = live; pendLiveAt[v] = 0; }
                else if (live != pendLive[v] || !pendLiveAt[v]) { pendLive[v] = live; pendLiveAt[v] = now; }
                else if (now - pendLiveAt[v] >= 40)
                {
                    if (!live) holdRound_[v].store(lastRound_[v].load());
                    liveStable_[v].store(live);
                    pendLiveAt[v] = 0;
                    swapped_[v].store(0);
                }
            }
            for (auto& t : screen_.taps())
            {
                const int p = t.first;
                if (p == P_NEW && L) { L->lane[genralink::LANE_DRUMS].newRequest.fetch_add(1); screen_.message(P_STATUS, on ? "NEW: new drums from the next bar" : "no GENRA CHOP"); }
                else if (p == P_UNDO && L) { L->lane[genralink::LANE_DRUMS].undoRequest.fetch_add(1); screen_.message(P_STATUS, on ? "UNDO: the last drums" : "no GENRA CHOP"); }
                else if (p == P_BASIC && nowMs() - armBasic_ > 3000) { armBasic_ = nowMs(); screen_.message(P_STATUS, "BASIC: tap again to reset all", 3000); }
                else if (p == P_BASIC)
                {
                    armBasic_ = 0;
                    for (int v = 0; v < kVoices; ++v) { set(P_RANDOM0 + v, 0.f); notifyHostParameterChanged(P_RANDOM0 + v); lastRandom[v] = 0.f; swapped_[v].store(0); }
                    for (int v = 0; v < kVoices; ++v)
                        for (int f = 0; f < NUM_FUNCS; ++f) setParam(P_FV0 + v * NUM_FUNCS + f, defaultValue(f));
                    syncSlots(true);
                    screen_.message(P_STATUS, "BASIC: samples and functions neutral");
                }
                else if (p == P_PRINT) print(L);
                else if (p == P_SAVESMP) saveSample();
                else if (p == P_SAVEKIT) saveKit();
                else if (p == P_LOADSMP) loadMarked();
                else if (p == P_CLEARSTEP && stepSel_ >= 0)
                {
                    for (auto& x : lock_[stepSel_ / kSteps][stepSel_ % kSteps]) x.store(std::numeric_limits<float>::quiet_NaN());
                    smpLock_[stepSel_ / kSteps][stepSel_ % kSteps].store(-1);
                    condLock_[stepSel_ / kSteps][stepSel_ % kSteps].store(0);
                    syncLocks(true);
                    screen_.message(P_STATUS, "CLEAR STEP: the main page's values", 2500);
                }
                else if (p >= P_RKINDTAP0 && p < P_RKINDTAP0 + kVoices)
                {
                    int v = p - P_RKINDTAP0;
                    setParam(P_RKIND0 + v, (float)((getInt(P_RKIND0 + v) + 1) % NUM_RKINDS));
                }
                else if (p >= P_STEPTAP0 && p < P_STEPTAP0 + kGrid)
                {
                    int i = p - P_STEPTAP0, v = i / 16;
                    stepSel_ = v * kSteps + clamp(getInt(P_BAR), 0, 3) * 16 + i % 16;
                    laneSel_ = -1;
                    syncLocks(true);
                    if (getInt(P_DRAW) == 0) toggleHit(v, stepSel_ % kSteps); // 0.15: DRAW: the tap sets / clears the hit
                }
                else if (p >= P_LANETAP0 && p < P_LANETAP0 + kVoices)
                {
                    laneSel_ = p - P_LANETAP0;
                    stepSel_ = -1;
                    syncLocks(true);
                }
                else if ((p == P_SMPPREV || p == P_SMPNEXT) && stepSel_ >= 0)
                {
                    int v = stepSel_ / kSteps, st = stepSel_ % kSteps;
                    Kit* k = kit_[v].load();
                    int n = k ? (int)k->samples.size() : 0;
                    if (n > 0)
                    {
                        // -1 (the voice's sample) .. n - 1
                        int cur = smpLock_[v][st].load(), next = cur + (p == P_SMPNEXT ? 1 : -1);
                        if (next >= n) next = -1;
                        if (next < -1) next = n - 1;
                        smpLock_[v][st].store((int16_t)next);
                        audition(v, next >= 0 ? next : k->base.load());
                    }
                }
                else if (p == P_SMPPREV || p == P_SMPNEXT)
                    screen_.message(P_STATUS, laneSel_ >= 0 ? "the lane's sample: page FOLDER" : "tap a step first", 2500);
                else if ((p == P_CONDPREV || p == P_CONDNEXT) && stepSel_ >= 0)
                {
                    int v = stepSel_ / kSteps, st = stepSel_ % kSteps;
                    int c = (condLock_[v][st].load() + (p == P_CONDNEXT ? 1 : NUM_CONDS - 1)) % NUM_CONDS;
                    condLock_[v][st].store((int8_t)c);
                }
                else if (p >= P_SKFTAP0 && p < P_SKFTAP0 + 2 * kStepKnobs)
                {
                    // STEPS: < > in a knob's field = what the knob sets
                    const int k = (p - P_SKFTAP0) / 2, dir = (p - P_SKFTAP0) % 2 ? 1 : NUM_SF - 1;
                    setParam(P_SKF0 + k, (float)((clamp(getInt(P_SKF0 + k), 0, NUM_SF - 1) + dir) % NUM_SF));
                }
                else if (p == P_HIT)
                {
                    if (stepSel_ < 0) screen_.message(P_STATUS, "tap a step first", 2500);
                    else toggleHit(stepSel_ / kSteps, stepSel_ % kSteps);
                }
                else if (p == P_CLEARALL && nowMs() - armClearAll_ > 3000) { armClearAll_ = nowMs(); screen_.message(P_STATUS, "CLEAR ALL: tap again", 3000); }
                else if (p == P_CLEARALL)
                {
                    armClearAll_ = 0;
                    // CLEAR ALL: every step's own values, and (with GENRA CHOP) every drum hit; UNDO there brings them back
                    for (auto& a : lock_) for (auto& b : a) for (auto& c : b) c.store(std::numeric_limits<float>::quiet_NaN());
                    for (auto& a : smpLock_) for (auto& b : a) b.store(-1);
                    for (auto& a : condLock_) for (auto& b : a) b.store(0);
                    syncLocks(true);
                    genralink::LinkData* Ld = link_.data();
                    if (Ld && linkAlive_)
                    {
                        uint32_t w = Ld->editW.load();
                        Ld->editCmd[w % genralink::LinkData::kEdits].store(genralink::kEditClearAll);
                        Ld->editW.store(w + 1, std::memory_order_release);
                        screen_.message(P_STATUS, getInt(P_AUTO) == 1 ? "CLEAR ALL (TRACK: erase on MPC)" : "CLEAR ALL: UNDO in GENRA CHOP", 4000);
                    }
                    else screen_.message(P_STATUS, "CLEAR ALL: values (no CHOP)", 4000);
                }
                else if (p == P_EMPTY && nowMs() - armEmpty_ > 3000) { armEmpty_ = nowMs(); screen_.message(P_LISTINFO, "EMPTY: tap again (the voice goes silent)", 3000); }
                else if (p == P_EMPTY)
                {
                    armEmpty_ = 0;
                    int v = clamp(getInt(P_VOICE), 0, kVoices - 1);
                    emptyVoice(v);
                    screen_.message(P_LISTINFO, std::string(kVoiceNames[v]) + ": EMPTY (silent)", 3000);
                }
                else if (p == P_LOADKIT) openKits();
                else if (p == P_COPYBAR) copyBar();
                else if (p == P_PASTE) pasteBar();
                else if (p == P_CLEARBAR) clearBar();
                else if (p == P_SKIN) screen_.message(P_DIAGINFO, "SKIN: coming soon", 4000);
                else if (p == P_DIAG)
                {
                    const std::string dir = genraDir();
                    if (dir.empty()) screen_.message(P_DIAGINFO, "DIAG: no drive to write to", 3000);
                    else if (!diag_.running())
                    {
                        diag_.start(dir, "GENRA SCHLAG 0.16 · LINK v3", &diagTr_, 120);
                        screen_.message(P_DIAGINFO, "DIAG: 2 min, now use the MPC", 4000);
                    }
                }
                else if (p >= P_ROLL0 && p < P_ROLL0 + kVoices)
                {
                    const int v = p - P_ROLL0; // 0.15: ROLL: a new roll, as if RANDOM were turned
                    seed_[v].fetch_add(1);
                    swapped_[v].store(0);
                    screen_.message(P_STATUS, std::string(kVoiceNames[v]) + ": ROLL", 1500);
                }
                else if (p >= P_SEGTAP0 && p < P_SEGTAP0 + kSegs)
                {
                    const std::string d = segPath(p - P_SEGTAP0);
                    if (!d.empty()) { browser_.open(d); previewPath_.clear(); }
                }
                else if (p >= P_CHIPTAP0 && p < P_CHIPTAP0 + kChips)
                {
                    const int f = chipFn_[p - P_CHIPTAP0];
                    if (f >= 0) setParam(P_SKF0, (float)f);
                }
                else if (p == P_AUTO)
                    screen_.message(P_STATUS, getInt(P_AUTO) == 0 ? "GENRA: GENRA's loop plays" : "TRACK: only the track's notes", 3500);
                else if (p == P_LEARN) pads_.startLearning();
                else if (p == P_BACK) { browser_.up(); previewPath_.clear(); }
                else if (p == P_DRVBTN) { browser_.drives(); previewPath_.clear(); }
                else if (p == P_PGUP) browser_.page(-1);
                else if (p == P_PGDN) browser_.page(1);
                else if (p == P_SCROLL) browser_.scroll(t.second > 0 ? 1 + (t.second - 1) / 6 : -1 - (-t.second - 1) / 6);
                else if (p == P_ROOTHERE)
                {
                    if (browser_.cwd().empty() || browser_.cwd() == "*") screen_.message(P_LISTINFO, "open a folder first");
                    else { root_ = browser_.cwd(); settings_.set("root", root_); settings_.save(); screen_.message(P_LISTINFO, "Root folder set"); }
                }
                else if (p >= P_FUNCTAP0 && p < P_FUNCTAP0 + kVoices * kSlots)
                {
                    int idx = p - P_FUNCTAP0;
                    int f = (getInt(P_FUNC0 + idx) + 1) % NUM_FUNCS;
                    setParam(P_FUNC0 + idx, (float)f);
                    syncSlots(false); // the knob now shows the new function's own value
                }
                else if (p >= P_PICK0 && p < P_PICK0 + kVoices)
                {
                    int v = p - P_PICK0;
                    set(P_VOICE, (float)v);
                    notifyHostParameterChanged(P_VOICE);
                    std::string f;
                    {
                        std::lock_guard<std::mutex> g(mtx_);
                        f = voiceFolder_[v];
                    }
                    if (!f.empty() && isDir(f)) browser_.open(f);
                    screen_.message(P_STATUS, std::string(kVoiceNames[v]) + " chosen: page FOLDER");
                }
                else if (p >= P_ROWTAP0 && p < P_ROWTAP0 + kRows) tapRow(p - P_ROWTAP0);
                else if (p >= P_VPREV0 && p < P_VNEXT0 + kVoices)
                {
                    const int v = (p - P_VPREV0) % kVoices;
                    stepSample(v, p >= P_VNEXT0 ? 1 : -1);
                }
            }
            if (pads_.finishIfDone()) screen_.message(P_LEARNINFO, "learned and saved", 3000);
            if (nowMs() - lastSlow > 1500)
            {
                lastSlow = nowMs();
                printList();
                if (L) genralink::Link::writeName(L->lane[genralink::LANE_DRUMS], "SCHLAG");
                if (browser_.cwd().empty()) browser_.list();
            }
            syncSlots(false);
            syncQ(!qlInit_);
            qlInit_ = true;
            // a voice chosen (page SCHLAG or FOLDER): the list opens its folder
            int vNow = clamp(getInt(P_VOICE), 0, kVoices - 1);
            if (vNow != lastVoice)
            {
                lastVoice = vNow;
                std::string f;
                {
                    std::lock_guard<std::mutex> g(mtx_);
                    f = voiceFolder_[vNow];
                }
                if (!f.empty() && isDir(f)) { browser_.open(f); previewPath_.clear(); }
            }
            if (stepSync_.exchange(false)) { stepSel_ = -1; syncLocks(true); }
            {
                // 0.15: FOLLOW: the page shows the bar that plays
                const int st = curStep_.load();
                if (getBool(P_FOLLOW) && st >= 0 && st / 16 != getInt(P_BAR)) setParam(P_BAR, (float)((st / 16) & 3));
                // drum edits (PASTE, CLEAR BAR) go to GENRA CHOP a few at a time: LINK's ring holds 16
                genralink::LinkData* Le = link_.data();
                for (int n = 0; n < 4 && !editsOut_.empty() && Le; ++n)
                {
                    uint32_t w = Le->editW.load();
                    Le->editCmd[w % genralink::LinkData::kEdits].store(editsOut_.front());
                    Le->editW.store(w + 1, std::memory_order_release);
                    editsOut_.erase(editsOut_.begin());
                }
            }
            syncLocks(false);
            syncStepKnobs();
            syncLocks(false);
            stepTexts();
            texts(L, on);
            cells();
            screen_.push();
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
        }
    }


    genraui::Screen screen_;
    genralink::Link link_;
    genradiag::Transport diagTr_;  // 0.16 DIAG: the host transport (audio thread writes)
    genradiag::Diag diag_;         // 0.16 DIAG: the capture (own thread)
    PadMap pads_;
    Settings settings_{"SCHLAG.txt"};
    Browser browser_;
    bool started_ = false;
    std::thread worker_;
    std::atomic<bool> quit_{false}, resyncReq_{false}, randomSync_{false};
    // audio thread
    Player players_[kVoices][2];
    Hit hits_[64];
    int nHits_ = 0;
    double sr_ = 44100.0, nextStep_ = 0.0, previewPos_ = 0.0;
    bool wasPlaying_ = false;
    bool audible_[kVoices] = {};
    float levelGain_[kVoices] = {1, 1, 1, 1, 1, 1, 1, 1};
    float gainSmooth_ = -1.f, limGain_ = 1.f;
    int padSeen_[kVoices] = {};
    // shared
    std::atomic<Kit*> kit_[kVoices], pendingKit_[kVoices];
    std::atomic<Kit*> retired_[kVoices][3];
    std::atomic<Sample*> preview_, previewPending_{nullptr};
    std::atomic<Sample*> previewRetired_[3];
    std::atomic<uint32_t> seed_[kVoices];
    std::atomic<float> lock_[kVoices][kSteps][kLocks];   // STEPS: a step's own values (NaN = none)
    std::atomic<int16_t> smpLock_[kVoices][kSteps];      // STEPS: a step's own sample (index in the kit), -1 none
    std::atomic<int8_t> condLock_[kVoices][kSteps];      // STEPS: a step's CONDITION (0 none)
    std::atomic<int> audition_{0};                       // (v * 1024 + sample) + 1: play once
    std::atomic<bool> readyNow_{false};
    int laneSel_ = -1;                                   // worker: STEPS editor on a whole lane (voice), -1 none
    bool linkAlive_ = false;
    int stepSel_ = -1;                                    // worker: the selected step (v * 64 + step), -1 none
    float shownLk_[kLocks] = {};
    std::atomic<bool> stepSync_{true};
    std::atomic<uint64_t> swapped_[kVoices];
    std::atomic<uint32_t> litMask_{0};
    std::atomic<int> ready_{-1}, inUse_{-1}, curStep_{-1};
    genralink::Loop loops_[3];
    int shownFunc_[kVoices * kSlots];
    float shownVal_[kVoices * kSlots] = {};
    int cur_ = -1; // audio thread: the loop that plays (index into loops_)
    // RANDOM (audio thread): the loop's hit positions, the track's note positions, the passes of the MPC sequence
    bool autoNow_ = true;
    uint64_t loopMask_[kVoices] = {}, seenCur_[kVoices] = {}, seenPrev_[kVoices] = {};
    uint32_t midiRound_[kVoices] = {}, wraps_ = 0;
    double lastPpq_ = -1.0;
    // RANDOM (shared): HOLD's roll, the last roll played, HOLD | LIVE as the worker saw it steady, positions for the state
    std::atomic<uint32_t> holdRound_[kVoices], lastRound_[kVoices];
    std::atomic<bool> liveStable_[kVoices], seenLoadReq_[kVoices];
    std::atomic<uint64_t> seenMask_[kVoices], seenLoad_[kVoices];
    // worker
    genralink::Loop loop_;
    KitInfo kitInfo_[kVoices];
    size_t kitBytes_[kVoices] = {};
    std::string root_, previewPath_;
    // worker + state (mtx_)
    std::mutex mtx_;
    std::string voiceFolder_[kVoices], voiceFile_[kVoices];
    bool loadReq_[kVoices] = {};
    int qlVoice_ = -1;                      // worker: Q-LINKS: FUNCTIONS
    float qlShown_[kQL] = {};
    bool rawWas_ = false, countIn_ = false;          // audio thread: the count-in (see process)
    double rawLastQ_ = 0.0;
    int skSel_ = -2, skFn_[kStepKnobs] = {-1, -1};   // worker: STEPS' two knobs
    float skShown_[kStepKnobs] = {};
    bool qlInit_ = false;
    int loadTotal_ = 0, loadMissing_ = 0;  // worker: the voices of this kit / project load, the folders missing
    std::string loadName_;
    std::string kitName_;                  // 0.15: worker: the kit loaded or saved last (KIT line)
    long long armBasic_ = 0, armClearAll_ = 0, armEmpty_ = 0; // 0.15: keys that need a second tap within 3 s
    struct BarClip { bool has = false; uint8_t hit[kVoices][16]; float lk[kVoices][16][kLocks]; int16_t smp[kVoices][16]; int8_t cond[kVoices][16]; };
    BarClip clip_;                         // 0.15: COPY BAR / PASTE
    std::vector<int32_t> editsOut_;
    int chipFn_[kChips] = {-1, -1, -1, -1};  // 0.15: the step function each chip stands for        // 0.15: drum edits waiting for room in LINK's small ring                 // worker: what to say when the kit is in
};

MPC_PLUGIN_ENTRY(Schlag)
