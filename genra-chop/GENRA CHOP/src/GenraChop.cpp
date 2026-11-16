// GENRA CHOP - the sample chopper of GENRA CHOP (VCV Rack) on the MPC, the chef of the GENRA plugins.
//
// SPDX-License-Identifier: GPL-3.0-only
//
// Pages (built by genra-chop/tools/build_skin.py), on the function keys:
//   BROWSER  path, ten rows to tap (folder: open; WAV: first tap = listen, second tap = load), ZURUECK,
//            LAUFWERKE, page buttons, SPEICHERORT HIER (SAVE WAV), LISTE (data wheel)
//   CHOP     header (sample, tempo, key, LINK), views SAMPLE / LOOP / CHOP, the display (a column waveform:
//            film strips), the chop strip (16 chops of the bank: number, chord, length, R = reverse, L = lock;
//            lit while playing, tap to play and select), the steps of the bar that plays, the lanes (drums, bass,
//            melody, pads: LINK lamp, notes, the companion's sound, NEW), STYLE, NEW ALL, MODUS, CHOPS, BANK,
//            UNDO, AUTO | PADS, CHOKE, REV, LOCK, PITCH / DECAY / FILTER / LEVEL of the selected chop, SAVE WAV
//   PRINT    PRINT (the chop track as a MIDI file), AUTO | PADS, the print folder and its newest prints
//   SETUP    LERNEN (the pads, shared with every GENRA plugin) and the status
//
// The loop: NEW ALL rolls all tracks with the GENRA CHOP engine (src/Core.cpp, Chop.cpp, Styles.cpp, copied from
// Camillowup/genra-chop). This plugin plays the CHOP track on the MPC's transport (Play / Stop of the MPC); a new
// loop starts at the next bar. Drums, bass, melody and pads go to the companions through LINK v3
// (common/genra_link.h). AUTO: the loop plays; PADS: only the notes of this track (after PRINT and the import of the
// MIDI file into the MPC's sequence).
//
// Threads: process() (audio) plays chops, the loop and the browser's preview and only touches atomics. A worker
// browses, loads, cuts, generates, writes files and makes the screen (common/genra_ui.h).
// Files: settings in <drive>/Genra/GENRA CHOP.txt, the pads in Genra/pads.txt, prints in Genra/PRINT
// (common/genra_files.h).

#include "mpc_plugin.h"
#include "Chop.hpp"
#include "../../common/genra_files.h"
#include "../../common/genra_link.h"
#include "../../common/genra_midiout.h"
#include "../../common/genra_diag.h"
#include "../../common/genra_ui.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
#include <thread>

using namespace mpc;
using namespace genrafiles;

namespace {

static const int kRows = 9;    // rows of the browser
static const int kPads = 16;   // chop slots per bank
static const int kCols = 124;  // columns of the display
static const int kLanes = 4;
static const int kPrintRows = 7;
static const int kColFrames = 48, kColH = 16;  // column strip: 3 states x 16 heights
static const int kSteps = hh::LEN;             // 64

static const char* const kCaptions[][2] = {
    {"genra", "GENRA"}, {"chop", "CHOP"}, {"sample", "SAMPLE"}, {"loop", "LOOP"}, {"chopv", "CHOP"},
    {"style", "STYLE"}, {"newall", "NEW ALL"}, {"new", "NEW"}, {"modus", "MODE"}, {"transient", "Transient"},
    {"equal", "Equal"}, {"chops", "CHOPS"}, {"minus", "-"}, {"plus", "+"}, {"bank", "BANK"},
    {"bankA", "A"}, {"bankB", "B"}, {"bankC", "C"}, {"bankD", "D"}, {"undo", "UNDO"}, {"savewav", "SAVE WAV"},
    {"drums", "DRUMS"}, {"bass", "BASS"}, {"mel", "MELODY"}, {"pads", "PADS"}, {"prev", "<"}, {"next", ">"}, {"stprev", "< STYLE"}, {"stnext", "STYLE >"},
    {"auto", "AUTO"}, {"padsmode", "PADS"}, {"choke", "CHOKE"}, {"rev", "REV"}, {"lock", "LOCK"},
    {"print", "PRINT"}, {"printtitle", "· PRINT"}, {"after", "After PRINT"}, {"autoinfo", "AUTO: the loop plays"},
    {"padsinfo", "PADS: you play the chops, GENRA the rest"}, {"printhere", "SAVE HERE"}, {"printstd", "STANDARD"},
    {"back", "BACK"}, {"drives", "DRIVES"}, {"pgup", "< PAGE"}, {"pgdn", "PAGE >"},
    {"savehere", "SAVE HERE"}, {"loaded", "Loaded:"}, {"learn", "LEARN"},
    {"help1", "Tap LEARN, then press"}, {"help2", "pads 1 to 16 in order"},
    {"help3", "(pad 1 = bottom left)"}, {"help4", "Used by every GENRA plugin"},
    {"r1", "Pad map"}, {"r2", "Drives"}, {"r3", "Writing"}, {"r4", "LINK"}, {"r5", "Analysis"},
    {"r6", "SAVE WAV to"}, {"r7", "MPC"}, {"r8", "Version"}, {"qlpage2", "Q-LINKS 2: GRID, PAT, OCT ..."}, {"diag", "DIAG"}, {"styletitle", "· STYLE"}, {"length", "LENGTH"},
    {"len1", "1 BAR"}, {"len2", "2 BARS"}, {"len4", "4 BARS"}, {"len8", "8 BARS"},
    {"era0", "80s"}, {"era1", "90s"}, {"era2", "2000s"}, {"era3", "2010s"}, {"era4", "2020s"}, {"lenauto", "AUTO"}, {"loadsmp", "LOAD SAMPLE"},
    {"clear", "CLEAR"}, {"rec", "● REC"}, {"chopsl", "CHOPS"}};
static const int kNumCaps = (int)(sizeof(kCaptions) / sizeof(kCaptions[0]));

// the keys that flash when tapped (in this order: FLASH 1.. in the skin)
static const char* const kFlashKeys[] = {"NEW ALL", "NEW DRUMS", "NEW BASS", "NEW MELODY", "NEW PADS", "NEW CHOPS", "UNDO",
    "SAVE WAV", "CHOPS MINUS", "CHOPS PLUS", "STYLE BACK", "STYLE NEXT", "PRINT", "LEARN", "BACK",
    "DRIVES", "PAGE BACK", "PAGE NEXT", "SAVE HERE", "PRINT HERE", "PRINT STANDARD", "LOAD SAMPLE", "CLEAR", "DIAG",
    "MODE BACK 1", "MODE BACK 2", "MODE BACK 3", "MODE BACK 4", "MODE NEXT 1", "MODE NEXT 2", "MODE NEXT 3", "MODE NEXT 4"};
static const int kNumFlash = (int)(sizeof(kFlashKeys) / sizeof(kFlashKeys[0]));

enum
{
    P_PITCH = 0, P_DECAY, P_FILTER, P_LEVEL,
    P_VOLUME, P_STYLE, P_MODE, P_CHOPS, P_BANK, P_VIEW, P_CHOKE, P_AUTO, P_LENGTH, P_REC,
    P_BASSOCT,                         // BASS OCT: -1 / 0 / +1 / +2 octaves on the bass line (at once)
    P_CHOPPAT,                         // PAT: the chop pattern's kind (AUTO, ORDER, STABS ...), acts with NEW (CHOPS / ALL)
    P_CHOPGRID,                        // GRID: AUTO (the style) or the chops on 1/4, 1/8, 1/16 (acts with NEW, like PAT)
    P_LANEMODE0,                       // per lane: DRUMS KIT/BREAK/BOTH, BASS FOLLOW.., MELODY STAB.., PADS SUSTAIN..
    P_DENS0 = P_LANEMODE0 + kLanes,    // per lane: DENSITY 0..100 % (50 = the style)
    // buttons (every tap flips them); the first kNumFlash have a flash, in kFlashKeys' order
    P_NEWALL = P_DENS0 + kLanes, P_NEW0,
    P_NEWCHOPS = P_NEW0 + kLanes, // NEW of the CHOPS row: built like the other NEW keys (with its flash)
    P_UNDO, P_SAVE, P_CHMINUS, P_CHPLUS, P_STPREV, P_STNEXT, P_PRINT, P_LEARN,
    P_BACK, P_DRVBTN, P_PGUP, P_PGDN, P_SAVEHERE, P_PRINTHERE, P_PRINTSTD, P_LOADSMP, P_CLEAR, P_DIAG,
    P_MODEPREV0, P_MODENEXT0 = P_MODEPREV0 + kLanes,
    P_REVBTN = P_MODENEXT0 + kLanes, P_LOCKBTN, P_OCTTAP, P_PATPREV, P_PATNEXT, P_SCROLL,
    P_ROWTAP0,
    P_SLOTTAP0 = P_ROWTAP0 + kRows,
    // texts
    P_PATH = P_SLOTTAP0 + kPads,
    P_LISTINFO, P_LOADED, P_HDR, P_CHOPSTXT, P_BARTXT, P_STATUS, P_LEARNINFO, P_MAPINFO, P_DRIVES, P_WRITE,
    P_LINKINFO, P_ANALYSE, P_SAVEDIRTXT, P_PRINTDIRTXT, P_PRINTINFO, P_PRINTSTATUS, P_HOSTINFO, P_STYLEHDR, P_CHOPTXT, P_VERSION, P_DIAGINFO,
    P_STYLENAME0,
    P_SLOTTXT0 = P_STYLENAME0 + hh::NUM_STYLES,
    P_SLOTLEN0 = P_SLOTTXT0 + kPads,
    P_LANETXT0 = P_SLOTLEN0 + kPads,
    P_ROWT0 = P_LANETXT0 + kLanes,
    P_PRINTROW0 = P_ROWT0 + kRows,
    P_CAP0 = P_PRINTROW0 + kPrintRows,
    // film strips
    P_COL0 = P_CAP0 + kNumCaps,
    P_SLOTLIT0 = P_COL0 + kCols,       // 0 off, 1 selected, 2 playing
    P_STEPLIT0 = P_SLOTLIT0 + kPads,
    P_CELL0 = P_STEPLIT0 + kPads,      // lane x step: 0 empty, 1 note, +2 the step that plays
    P_LAMP0 = P_CELL0 + kLanes * kPads,
    P_REVLIT = P_LAMP0 + kLanes, P_LOCKLIT,
    P_FLASH0,
    P_COUNT = P_FLASH0 + kNumFlash
};

static const int kVoices = 12;
static const int kFadeIn = 132;   // 0.12: about 3 ms (was 0.7 ms: clicks at the chop start)
static const int kFadeOut = 96;
static const int kChokeFade = 220; // 0.12: a choked chop fades over about 5 ms (was 1 ms)

struct Voice
{
    bool active = false, seq = false, rev = false;
    int chop = -1;
    double pos = 0.0, a = 0.0, b = 0.0, rate = 1.0;
    double gate = -1.0;
    double decayLen = 0.0;
    float gain = 1.f;
    float fa = 1.f;
    float zl1 = 0.f, zl2 = 0.f, zr1 = 0.f, zr2 = 0.f;
    int age = 0;
    int release = -1;
    int relLen = kFadeOut;  // the fade-out of this release (a choke fades longer)
};

struct Trig { double at; int chop; float vel; int pitch; bool rev; double gate; };

struct Seq
{
    hh::ChopEv ev[hh::LEN];
    // MIDI OUT: the other lanes of the loop (bass and melody lengths in steps, slides already reaching the next note)
    hh::Note bass[hh::LEN], mel[hh::LEN];
    float bassLen[hh::LEN] = {}, melLen[hh::LEN] = {};
    hh::PadEv pad[hh::LEN];
    hh::Hit drum[hh::DRUMS][hh::LEN];
    float swing = 0.5f;
};

// A built-in test loop (2 bars at 92 BPM, F#m7 / Bm7 stabs with a kick and hats) until a WAV is loaded.
hh::SampleData makeBuiltinLoop()
{
    hh::SampleData s;
    const float sr = 44100.f, bpm = 92.f;
    const size_t beat = (size_t)(sr * 60.f / bpm);
    const size_t n = beat * 8;
    s.L.assign(n, 0.f);
    s.R.assign(n, 0.f);
    s.sampleRate = sr;
    s.name = "Test loop (built in)";
    const float chords[2][4] = {{54, 57, 61, 64}, {59, 62, 66, 69}};
    uint32_t rng = 12345;
    for (int b = 0; b < 8; ++b)
    {
        size_t at = (size_t)b * beat;
        if (b % 2 == 0)
            for (size_t i = 0; i < beat / 2 && at + i < n; ++i)
            {
                float t = i / sr;
                float v = 0.9f * std::sin(2.f * 3.14159265f * (50.f + 80.f * std::exp(-t * 30.f)) * t) * std::exp(-t * 9.f);
                s.L[at + i] += v; s.R[at + i] += v;
            }
        const float* c = chords[(b / 4) % 2];
        for (size_t i = 0; i < beat * 3 / 4 && at + i < n; ++i)
        {
            float t = i / sr, env = std::exp(-t * 4.f) * 0.12f, v = 0.f;
            for (int k = 0; k < 4; ++k)
            {
                float f = 440.f * std::pow(2.f, (c[k] - 69.f) / 12.f);
                v += std::sin(2.f * 3.14159265f * f * t) + 0.3f * std::sin(4.f * 3.14159265f * f * t);
            }
            s.L[at + i] += v * env; s.R[at + i] += v * env * 0.9f;
        }
        size_t h = at + beat / 2;
        for (size_t i = 0; i < 2000 && h + i < n; ++i)
        {
            rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
            float v = ((rng >> 8) * (1.f / 8388608.f) - 1.f) * 0.15f * std::exp(-(float)i / 300.f);
            s.L[h + i] += v; s.R[h + i] += v;
        }
    }
    return s;
}

std::string lengthText(float secs, float bpm)
{
    if (secs <= 0.f || bpm <= 0.f) return std::string();
    int n = std::max(1, (int)std::lround(secs * bpm / 60.f * 4.f)); // in 16ths
    char b[24];
    if (n % 16 == 0) std::snprintf(b, sizeof b, "%d T", n / 16);
    else if (n % 4 == 0) std::snprintf(b, sizeof b, "%d/4", n / 4);
    else if (n % 2 == 0) std::snprintf(b, sizeof b, "%d/8", n / 2);
    else std::snprintf(b, sizeof b, "%d/16", n);
    return b;
}

std::string styleLabel(int i)
{
    const hh::Style& s = hh::STYLES[i];
    return std::string(hh::ERA_NAMES[s.era]) + " · " + s.shortName;
}

} // namespace

class GenraChop : public Plugin
{
public:
    GenraChop() : Plugin(makeInfo()), screen_(*this)
    {
        addParam(intParam("pitch", "PITCH", -12, 12, 0, "st"));
        addParam(floatParam("decay", "DECAY", 0.f, 100.f, 100.f, "%", 0));
        addParam(floatParam("filter", "FILTER", 0.f, 100.f, 100.f, "", 0));
        addParam(floatParam("level", "LEVEL", -24.f, 6.f, 0.f, "dB", 1));
        addParam(floatParam("volume", "VOLUME", -40.f, 6.f, 0.f, "dB", 1));
        std::vector<std::string> styles;
        for (int i = 0; i < hh::NUM_STYLES; ++i) styles.push_back(styleLabel(i));
        addParam(choiceParam("style", "STYLE", styles, 5));
        addParam(choiceParam("mode", "MODE", {"Transient", "Equal"}, 0));
        addParam(intParam("chops", "CHOPS", 4, 64, 16));
        addParam(choiceParam("bank", "BANK", {"A", "B", "C", "D"}, 0));
        addParam(choiceParam("view", "VIEW", {"Sample", "Loop", "Chop"}, 0));
        addParam(boolParam("choke", "CHOKE", true));
        addParam(choiceParam("auto", "AUTO PADS", {"Auto", "Pads"}, 0));
        addParam(choiceParam("length", "LENGTH", {"Auto", "1 bar", "2 bars", "4 bars", "8 bars"}, 0));
        addParam(boolParam("rec", "REC", false));
        addParam(choiceParam("bassoct", "BASS OCT", {"OCT -1", "OCT 0", "OCT +1", "OCT +2"}, 1));
        addParam(choiceParam("choppat", "CHOP PAT", {hh::CHOP_PAT_NAMES, hh::CHOP_PAT_NAMES + hh::NUM_CHOP_PATS}, 0));
        addParam(choiceParam("chopgrid", "GRID", {"AUTO", "1/4", "1/8", "1/16"}, 0));
        addParam(choiceParam("drummode", "DRUM MODE", {hh::GROOVE_NAMES, hh::GROOVE_NAMES + hh::NUM_GROOVES}, 0)); // the groove (2.7.0)
        addParam(choiceParam("bassmode", "BASS MODE", {hh::BASS_MODES, hh::BASS_MODES + 4}, 0));
        addParam(choiceParam("melmode", "MELODY MODE", {hh::MEL_MODES, hh::MEL_MODES + 4}, 0));
        addParam(choiceParam("padmode", "PADS MODE", {hh::PAD_MODES, hh::PAD_MODES + 3}, 0));
        for (const char* d : {"DENS DRUMS", "DENS BASS", "DENS MELODY", "DENS PADS"})
        {
            std::string id = d;
            for (char& c : id) c = c == ' ' ? '_' : (char)::tolower((unsigned char)c);
            addParam(floatParam(id.c_str(), d, 0.f, 100.f, 50.f, "%", 0));
        }
        const char* lanes[kLanes] = {"DRUMS", "BASS", "MELODY", "PADS"};
        addButton("NEW ALL");
        for (int k = 0; k < kLanes; ++k) addButton((std::string("NEW ") + lanes[k]).c_str());
        addButton("NEW CHOPS");
        for (const char* b : {"UNDO", "SAVE WAV", "CHOPS MINUS", "CHOPS PLUS", "STYLE BACK", "STYLE NEXT", "PRINT",
                              "LEARN", "BACK", "DRIVES", "PAGE BACK", "PAGE NEXT", "SAVE HERE",
                              "PRINT HERE", "PRINT STANDARD", "LOAD SAMPLE", "CLEAR", "DIAG"})
            addButton(b);
        for (const char* d : {"MODE BACK", "MODE NEXT"})
            for (int k = 0; k < kLanes; ++k) addButton((std::string(d) + " " + std::to_string(k + 1)).c_str());
        addButton("REV");
        addButton("LOCK");
        addButton("OCT TAP");
        addButton("PAT BACK");
        addButton("PAT NEXT");
        addParam(intParam("scroll", "LIST", 0, 1000, 500));
        for (int i = 0; i < kRows; ++i) addButton(("TIPP " + std::to_string(i + 1)).c_str());
        for (int i = 0; i < kPads; ++i) addButton(("SLOT " + std::to_string(i + 1)).c_str());
        for (const char* t : {"PFAD", "LISTINFO", "GELADEN", "KOPF", "CHOPZAHL", "TAKT", "STATUS", "LERNINFO",
                              "BELEGUNG", "LAUFWERKSLISTE", "SCHREIBEN", "LINKINFO", "ANALYSE", "SPEICHERORT",
                              "PRINTORDNER", "PRINTINFO", "PRINTSTATUS", "HOSTINFO", "STYLEKOPF", "CHOPTEXT", "VERSION", "DIAGINFO"})
            addText(t);
        for (int i = 0; i < hh::NUM_STYLES; ++i) addText(("STYLENAME " + std::to_string(i + 1)).c_str());
        for (int i = 0; i < kPads; ++i) addText(("SLOTTEXT " + std::to_string(i + 1)).c_str());
        for (int i = 0; i < kPads; ++i) addText(("SLOTLAENGE " + std::to_string(i + 1)).c_str());
        for (int i = 0; i < kLanes; ++i) addText(("SPURTEXT " + std::to_string(i + 1)).c_str());
        for (int i = 0; i < kRows; ++i) addText(("ZEILE " + std::to_string(i + 1)).c_str());
        for (int i = 0; i < kPrintRows; ++i) addText(("PRINTZEILE " + std::to_string(i + 1)).c_str());
        for (const auto& c : kCaptions) addText((std::string("CAP ") + c[0]).c_str());
        for (int i = 0; i < kCols; ++i) addText(("SPALTE " + std::to_string(i + 1)).c_str());
        for (int i = 0; i < kPads; ++i) addText(("SLOTLICHT " + std::to_string(i + 1)).c_str());
        for (int i = 0; i < kPads; ++i) addText(("SCHRITT " + std::to_string(i + 1)).c_str());
        for (int k = 0; k < kLanes; ++k)
            for (int i = 0; i < kPads; ++i) addText(("ZELLE " + std::to_string(k + 1) + " " + std::to_string(i + 1)).c_str());
        for (int k = 0; k < kLanes; ++k) addText(("LAMPE " + std::to_string(k + 1)).c_str());
        addText("REVLICHT");
        addText("LOCKLICHT");
        for (int i = 0; i < kNumFlash; ++i) addText(("BLITZ " + std::to_string(i + 1)).c_str());
        if (numParams() != P_COUNT) std::abort(); // the enum and the list above must agree (host_sim catches it)

        screen_.init();
        for (int i = P_PATH; i < P_COL0; ++i) screen_.text(i);
        for (int i = 0; i < kNumCaps; ++i) screen_.text(P_CAP0 + i, kCaptions[i][1]);
        for (int i = 0; i < hh::NUM_STYLES; ++i) screen_.text(P_STYLENAME0 + i, hh::STYLES[i].shortName);
        for (int i = 0; i < kCols; ++i) screen_.strip(P_COL0 + i, kColFrames);
        for (int i = 0; i < kPads; ++i) { screen_.strip(P_SLOTLIT0 + i, 3); screen_.strip(P_STEPLIT0 + i, 2); }
        for (int i = 0; i < kLanes * kPads; ++i) screen_.strip(P_CELL0 + i, 4);
        for (int i = 0; i < kLanes; ++i) screen_.strip(P_LAMP0 + i, 2);
        screen_.strip(P_REVLIT, 2);
        screen_.strip(P_LOCKLIT, 2);
        screen_.setText(P_VERSION, "GENRA CHOP 0.15 · LINK v3");
        for (int p = P_NEWALL; p < P_ROWTAP0; ++p)
        {
            if (p == P_SCROLL) continue;
            int f = p - P_NEWALL;
            screen_.button(p, p < P_REVBTN && f < kNumFlash ? P_FLASH0 + f : -1);
        }
        for (int i = 0; i < kRows; ++i) screen_.button(P_ROWTAP0 + i);
        screen_.watch(P_SCROLL);
        screen_.watch(P_AUTO);
        screen_.watch(P_REC);

        for (auto& r : retired_) r.store(nullptr);
        hostCycle_[0].store(0.0);
        hostCycle_[1].store(0.0);
        for (auto& r : seqRetired_) r.store(nullptr);
        for (auto& r : previewRetired_) r.store(nullptr);
        for (int c = 0; c < hh::PADS; ++c)
        {
            cPitch_[c].store(0.f); cDecay_[c].store(100.f); cFilter_[c].store(100.f); cLevel_[c].store(0.f);
            cRev_[c].store(false);
        }
    }

    ~GenraChop() override
    {
        quit_.store(true);
        if (worker_.joinable()) worker_.join();
        delete cur_;
        delete pending_.exchange(nullptr);
        for (auto& r : retired_) delete r.exchange(nullptr);
        delete seq_;
        delete seqPending_.exchange(nullptr);
        for (auto& r : seqRetired_) delete r.exchange(nullptr);
        delete preview_;
        delete previewPending_.exchange(nullptr);
        for (auto& r : previewRetired_) delete r.exchange(nullptr);
    }

    static PluginInfo makeInfo()
    {
        PluginInfo i;
        i.name = "GENRA CHOP";
        i.vendor = "Genra";
        i.uniqueId = MPC_FOURCC('G', 'n', 'C', 'h');
        i.version = 800;
        i.isInstrument = true;
        i.numInputs = 0;
        i.numOutputs = 2;
        return i;
    }

    // ------------------------------------------------------------------ lifecycle
    void prepare(double sampleRate, int) override
    {
        sr_ = sampleRate;
        if (!started_)
        {
            started_ = true;
            link_.open();
            pads_.load();
            midi_.open("GENRA CHOP");
            settings_.load();
            set(P_BANK, 0.f);
            bankReset_.store(true);
            seed_ = (uint32_t)std::chrono::steady_clock::now().time_since_epoch().count() | 1u;
            src_ = makeBuiltinLoop();
            cutAndPublish(true);
            worker_ = std::thread([this] { workerMain(); });
        }
    }

    void reset() override
    {
        for (Voice& v : voices_) v.active = false;
        nTrig_ = 0;
        gainSmooth_ = -1.f;
        wasPlaying_ = false;
        for (int k = 0; k < kPads; ++k) slotTapSeen_[k] = getInt(P_SLOTTAP0 + k);
    }

    // ------------------------------------------------------------------ audio
    void process(ProcessContext& ctx) override
    {
        if (hh::ChopSet* n = pending_.exchange(nullptr, std::memory_order_acq_rel))
        {
            for (Voice& v : voices_) v.active = false;
            nTrig_ = 0;
            if (cur_) retire(cur_, retired_);
            cur_ = n;
        }
        if (hh::SampleData* p = previewPending_.exchange(nullptr, std::memory_order_acq_rel))
        {
            if (preview_) retire(preview_, previewRetired_);
            preview_ = p;
            previewPos_ = 0.0;
            previewFade_ = -1;
        }
        if (previewStop_.exchange(false) && preview_) previewFade_ = kFadeOut * 4;

        const Transport& tr = ctx.transport;
        // DIAG: what the host reports, for the capture (genra_diag.h)
        diagTr_.ppq.store(tr.ppqPos, std::memory_order_relaxed);
        diagTr_.samplePos.store(tr.samplePos, std::memory_order_relaxed);
        diagTr_.tempo.store(tr.bpm, std::memory_order_relaxed);
        diagTr_.barStart.store(tr.barStartPpq, std::memory_order_relaxed);
        diagTr_.flags.store((tr.valid ? 1 : 0) | (tr.playing ? 2 : 0) | (tr.recording ? 4 : 0) | (tr.ppqValid ? 8 : 0) |
                                (tr.cycling ? 16 : 0), std::memory_order_relaxed);
        const bool autoMode = getInt(P_AUTO) == 0;
        // the MPC's count-in (Overdub / Rec): stay silent until the one. 0.12: how the MPC reports it is unknown, so
        // three signs count: a position before the start, a negative sample position, a position that stands still
        // while "playing". What the MPC reports at each start is measured for the SETUP page (row MPC).
        const bool rawPlay = tr.valid && tr.playing && tr.ppqValid;
        if (rawPlay && !diagWas_)
        {
            diagQ0_.store(tr.ppqPos, std::memory_order_relaxed);
            diagS0_.store(tr.samplePos, std::memory_order_relaxed);
            diagRec_.store(tr.recording ? 1 : 0, std::memory_order_relaxed);
            diagJumpFrom_.store(-1.0, std::memory_order_relaxed);
            diagJumpTo_.store(-1.0, std::memory_order_relaxed);
            diagFrozen_.store(0, std::memory_order_relaxed);
            diagBlocks_ = 0;
            diagCount_.fetch_add(1, std::memory_order_relaxed);
        }
        bool frozen = false;
        if (rawPlay && diagWas_)
        {
            frozen = std::fabs(tr.ppqPos - diagLastQ_) < 1e-9;
            if (frozen) diagFrozen_.fetch_add(1, std::memory_order_relaxed);
            if (tr.ppqPos < diagLastQ_ - 0.25 && diagJumpFrom_.load(std::memory_order_relaxed) < 0.0 && diagBlocks_ < 4000)
            {
                diagJumpFrom_.store(diagLastQ_, std::memory_order_relaxed);
                diagJumpTo_.store(tr.ppqPos, std::memory_order_relaxed);
            }
            ++diagBlocks_;
        }
        if (rawPlay && tr.recording) diagRec_.store(diagRec_.load(std::memory_order_relaxed) | 2, std::memory_order_relaxed);
        diagWas_ = rawPlay;
        diagLastQ_ = tr.ppqPos;
        // 0.13 (DIAG 16.11.): the MPC's count-in runs PLAY + REC with the position a bar or more past the bar start it
        // reports (e.g. 5.3 .. 9.3 beats while the bar start stays 0), then jumps to the one
        const double beatsPerBar = tr.timeSigDenominator > 0 ? 4.0 * tr.timeSigNumerator / tr.timeSigDenominator : 4.0;
        const bool pastBar = tr.recording && tr.ppqPos - tr.barStartPpq > beatsPerBar + 0.5;
        const bool countIn = tr.ppqPos < -0.01 || tr.samplePos < -1.0 || frozen || pastBar;
        const bool playing = rawPlay && !countIn;
        if (!playing || !seq_) swapSeq();
        hostCycling_.store(tr.cycling, std::memory_order_relaxed);
        hostCycle_[0].store(tr.cycleStartPpq, std::memory_order_relaxed);
        hostCycle_[1].store(tr.cycleEndPpq, std::memory_order_relaxed);
        hostBar_.store(tr.barStartPpq, std::memory_order_relaxed);
        hostSig_.store(tr.timeSigNumerator * 100 + tr.timeSigDenominator, std::memory_order_relaxed);
        hostPlaying_.store(playing, std::memory_order_relaxed);
        // LENGTH: AUTO follows the MPC sequence's loop (the cycle it reports), else 1 / 2 / 4 / 8 bars
        {
            static const long k[5] = {64, 16, 32, 64, 128};
            const int lm = clamp(getInt(P_LENGTH), 0, 4);
            loopLen_ = k[lm];
            loopStart_ = 0;
            if (lm == 0 && tr.cycling && tr.cycleEndPpq > tr.cycleStartPpq + 0.2)
            {
                loopLen_ = clamp((long)std::lround((tr.cycleEndPpq - tr.cycleStartPpq) * 4.0), 4L, 1024L);
                loopStart_ = (long)std::lround(tr.cycleStartPpq * 4.0);
            }
            loopLenShown_.store((int)loopLen_, std::memory_order_relaxed);
            loopStartShown_.store(loopStart_, std::memory_order_relaxed);
        }

        genralink::LinkData* L = link_.data();
        const double bpm = tr.tempoValid && tr.bpm > 1.0 ? tr.bpm : 120.0;
        if (tr.tempoValid) hostBpmX100_.store((int)std::lround(tr.bpm * 100.0), std::memory_order_relaxed);
        if (L)
        {
            L->loopSteps.store((int32_t)loopLen_, std::memory_order_relaxed);
            L->loopStart.store((int32_t)loopStart_, std::memory_order_relaxed);
            L->writerBeat.fetch_add(1, std::memory_order_relaxed);
            if (tr.tempoValid) L->bpmX100.store((int32_t)std::lround(tr.bpm * 100.0), std::memory_order_relaxed);
        }
        const int bank = clamp(getInt(P_BANK), 0, 3);

        for (int k = 0; k < kPads; ++k)
        {
            int v = getInt(P_SLOTTAP0 + k);
            if (v != slotTapSeen_[k])
            {
                slotTapSeen_[k] = v;
                int chop = k + kPads * bank;
                selReq_.store(chop, std::memory_order_relaxed);
                startVoice(chop, 0.8f, 0, false, -1.0, false);
            }
        }

        const double spStep = sr_ * 60.0 / bpm / 4.0;
        // REC: armed + playing; the first bar after the start (or after arming) is the count-in
        const bool recOn = getBool(P_REC);
        const double recQ0 = playing ? tr.ppqPos * 4.0 : 0.0;
        if (recOn && playing && (!recWas_ || !wasPlaying_))
            recFrom_ = std::ceil(recQ0 / hh::STEPS - 1e-6) * hh::STEPS + hh::STEPS;
        recWas_ = recOn && playing;
        const bool recording = recOn && playing;
        recState_.store(!recOn ? 0 : !playing ? 1 : recQ0 < recFrom_ - 0.5 ? 2 : 3, std::memory_order_relaxed);
        recFromShared_.store(recFrom_, std::memory_order_relaxed);
        if (playing && seq_)
        {
            const double q0 = tr.ppqPos * 4.0;
            const double q1 = q0 + ctx.numFrames / spStep;
            if (!wasPlaying_ || nextStep_ < q0 - 1.0 || nextStep_ > q1 + 1.0)
            {
                // a start (also right after the count-in, where the MPC reports 0.001 .. 0.005): a step that began a
                // moment ago still plays, at once
                nextStep_ = std::floor(q0 + 1e-6);
                if (q0 - nextStep_ > 0.25) nextStep_ += 1.0;
            }
            const long len = loopLen_;
            while (nextStep_ < q1)
            {
                long n = (long)std::llround(nextStep_) - loopStart_;
                // LENGTH: the loop's first bars (1, 2, 3 ..), then from the start; longer: the 4 bars again.
                // A new loop (NEW, STYLE) starts at the end of the pattern.
                long pos = ((n % len) + len) % len;
                int s = (int)(pos % kSteps);
                if (pos == 0 || swapNow_.exchange(false)) swapSeq();
                curStep_.store(s, std::memory_order_relaxed);
                if (L) L->step.store(s, std::memory_order_relaxed);
                double at = std::max(0.0, (nextStep_ - q0) * spStep); // a step begun a moment ago: at once
                if (seq_)
                {
                    // 0.15: PADS silences only the chop pattern (the pads play the chops); bass, melody, pads and
                    // drums go on, also on MIDI OUT
                    const uint32_t pass = (uint32_t)((n - pos) / len);
                    if (autoMode) scheduleStep(seq_->ev[s], s, pass, at, spStep, seq_->swing);
                    scheduleMidi(*seq_, s, pass, at, spStep);
                }
                nextStep_ += 1.0;
            }
            wasPlaying_ = true;
        }
        else
        {
            if (wasPlaying_)
                for (Voice& v : voices_) if (v.active && v.seq && v.release < 0) v.release = kFadeOut;
            nTrig_ = 0;
            if (wasPlaying_) { midi_.allOff(); nMidi_ = 0; }
            wasPlaying_ = false;
            curStep_.store(-1, std::memory_order_relaxed);
            if (L) L->step.store(-1, std::memory_order_relaxed);
        }

        float target = std::pow(10.f, get(P_VOLUME) / 20.f);
        if (gainSmooth_ < 0.f) gainSmooth_ = target;
        float* outL = ctx.outputs[0];
        float* outR = ctx.numOutputs > 1 ? ctx.outputs[1] : nullptr;
        int ev = 0;
        const double previewRate = preview_ ? (double)preview_->sampleRate / sr_ : 1.0;
        for (int i = 0; i < ctx.numFrames; ++i)
        {
            while (ev < ctx.numMidiEvents && ctx.midi[ev].sampleOffset <= i)
            {
                const MidiEvent& e = ctx.midi[ev++];
                if (e.isNoteOn())
                {
                    int chop = noteOn(e.note(), e.velocity() / 127.f, bank, true);
                    if (chop >= 0 && recording) recordHit(chop, e.velocity() / 127.f, recQ0 + i / spStep);
                }
                else if (e.isAllNotesOff())
                    for (Voice& v : voices_) if (v.active && v.release < 0) v.release = kFadeOut;
            }
            for (int t = 0; t < nTrig_;)
            {
                if (trig_[t].at <= (double)i)
                {
                    const Trig& g = trig_[t];
                    startVoice(g.chop, g.vel, g.pitch, g.rev, g.gate, true);
                    trig_[t] = trig_[--nTrig_];
                }
                else ++t;
            }
            gainSmooth_ += (target - gainSmooth_) * 0.002f;
            float l = 0.f, r = 0.f;
            if (cur_) renderVoices(l, r);
            if (preview_ && previewFade_ != 0)
            {
                size_t k = (size_t)previewPos_;
                const std::vector<float>& pl = preview_->L;
                const std::vector<float>& pr = preview_->R.empty() ? preview_->L : preview_->R;
                if (k + 1 < pl.size())
                {
                    float f = (float)(previewPos_ - (double)k), g = 0.5f;
                    if (previewFade_ > 0) { g *= (float)previewFade_ / (kFadeOut * 4); --previewFade_; }
                    l += (pl[k] + (pl[k + 1] - pl[k]) * f) * g;
                    r += (pr[k] + (pr[k + 1] - pr[k]) * f) * g;
                    previewPos_ += previewRate;
                }
            }
            outL[i] = l * gainSmooth_;
            if (outR) outR[i] = r * gainSmooth_;
        }
        for (int t = 0; t < nTrig_; ++t) trig_[t].at -= ctx.numFrames;
        flushMidi(ctx.numFrames);
        uint64_t mask = 0;
        for (const Voice& v : voices_) if (v.active && v.release < 0 && v.chop >= 0 && v.chop < 64) mask |= (uint64_t)1 << v.chop;
        playingMask_.store(mask, std::memory_order_relaxed);
    }

    // ------------------------------------------------------------------ state
    void saveExtraState(std::vector<uint8_t>& out) override
    {
        std::lock_guard<std::mutex> g(mtx_);
        std::string head = "GC2\n" + loadedPath_ + "\n";
        out.assign(head.begin(), head.end());
        auto put = [&out](const void* p, size_t n) { const uint8_t* b = (const uint8_t*)p; out.insert(out.end(), b, b + n); };
        uint32_t size = (uint32_t)sizeof(hh::Pattern);
        put(&size, 4);
        put(&savedPattern_, sizeof(hh::Pattern));
        for (int c = 0; c < hh::PADS; ++c)
        {
            float v[6] = {cPitch_[c].load(), cDecay_[c].load(), cFilter_[c].load(), cLevel_[c].load(),
                          cRev_[c].load() ? 1.f : 0.f, padSet_[c].lock ? 1.f : 0.f};
            put(v, sizeof v);
        }
    }

    void loadExtraState(const uint8_t* data, size_t size) override
    {
        std::lock_guard<std::mutex> g(mtx_);
        stateLoaded_.store(true);
        std::string s(reinterpret_cast<const char*>(data), size);
        bool v2 = s.compare(0, 4, "GC2\n") == 0;
        if (!v2 && s.compare(0, 4, "GC1\n") != 0) return;
        size_t e = s.find('\n', 4);
        if (e == std::string::npos) return;
        requestedPath_ = s.substr(4, e - 4);
        size_t pos = e + 1;
        uint32_t psize = 0;
        if (pos + 4 <= size) std::memcpy(&psize, data + pos, 4);
        pos += 4;
        const size_t per = v2 ? 24 : 16;
        if (psize == sizeof(hh::Pattern) && pos + psize + hh::PADS * per <= size)
        {
            std::memcpy(&restoredPattern_, data + pos, psize);
            restored_ = true;
            pos += psize;
            for (int c = 0; c < hh::PADS; ++c)
            {
                float v[6] = {0, 100, 100, 0, 0, 0};
                std::memcpy(v, data + pos + (size_t)c * per, per);
                cPitch_[c].store(v[0]); cDecay_[c].store(v[1]); cFilter_[c].store(v[2]); cLevel_[c].store(v[3]);
                cRev_[c].store(v[4] > 0.5f);
                restoredLock_[c] = v[5] > 0.5f;
            }
        }
        pathRequest_ = true; // (the saved BANK stays)
        resyncReq_.store(true);
    }

    bool formatValue(int index, float plain, std::string& out) override
    {
        char b[32];
        if (index == P_FILTER)
        {
            if (plain >= 99.5f) { out = "open"; return true; }
            float hz = cutoffHz(plain);
            if (hz >= 1000.f) std::snprintf(b, sizeof b, "LP %.1fk", hz / 1000.f);
            else std::snprintf(b, sizeof b, "LP %.0f", hz);
            out = b;
            return true;
        }
        if (index == P_PITCH) { std::snprintf(b, sizeof b, "%+d st", (int)std::lround(plain)); out = b; return true; }
        if (index == P_DECAY) { std::snprintf(b, sizeof b, plain >= 99.5f ? "full" : "%.0f %%", plain); out = b; return true; }
        if (index == P_LEVEL) { std::snprintf(b, sizeof b, "%+.1f dB", plain); out = b; return true; }
        if (index == P_SCROLL) { listInfoText(out); return true; }
        if (index >= P_COL0) return false;
        return screen_.format(index, out);
    }

private:
    static float cutoffHz(float v) { return 120.f * std::pow(2.f, v / 100.f * 7.2f); }

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
    void listInfoText(std::string& out)
    {
        screen_.format(P_LISTINFO, out);
    }

    // ------------------------------------------------------------------ audio helpers
    // a pad: plays its chop and always selects it (REV, LOCK, PITCH .. act on the pad played last)
    int noteOn(int note, float vel, int bank, bool select)
    {
        if (pads_.learnNote(note)) return -1;
        int pad = pads_.padOf(note);
        if (pad < 0) return -1;
        int chop = pad + kPads * bank;
        if (select) selReq_.store(chop, std::memory_order_relaxed);
        startVoice(chop, vel, 0, false, -1.0, false);
        return chop;
    }

    // REC (audio thread): a pad hit, its position in steps on the MPC's time line, for the worker
    void recordHit(int chop, float vel, double q)
    {
        if (q < recFrom_ - 0.5) return; // still the count-in
        int w = recW_.load(std::memory_order_relaxed);
        if (w - recR_.load(std::memory_order_acquire) >= kRecQ) return;
        recQ_[w % kRecQ] = {chop, vel, q};
        recW_.store(w + 1, std::memory_order_release);
    }

    void scheduleStep(const hh::ChopEv& e, int s, uint32_t round, double at, double spStep, float swing)
    {
        if (e.pad < 0 || !cur_ || e.pad >= cur_->info.count) return;
        if (e.cond == 1 && (round & 1u)) return;
        if (e.chance < 1.f && hh::hash01((uint32_t)s, round, 77u) >= e.chance) return;
        if (s & 1) at += (swing - 0.5f) * 2.f * spStep;
        if (e.off > 0.f) at += e.off * spStep;
        int r = std::max(1, (int)e.ratchet);
        double gate = std::max(0.25, (double)e.len) * spStep;
        for (int k = 0; k < r; ++k)
        {
            if (nTrig_ >= (int)(sizeof(trig_) / sizeof(trig_[0]))) return;
            Trig& t = trig_[nTrig_++];
            t.at = at + k * spStep / r;
            t.chop = e.pad;
            t.vel = e.vel * (k ? 0.8f : 1.f);
            t.pitch = e.pitch;
            t.rev = e.rev;
            t.gate = r > 1 ? spStep / r : gate;
            // channel 1: the chop as the note of its pad (the pad map of the GENRA plugins)
            queueMidi(t.at, t.at + t.gate * 0.95, kChChops, pads_.noteOf(e.pad % kPads), t.vel);
        }
    }

    // ------------------------------------------------------------------ MIDI OUT (GENRA CHOP OUT)
    // channel 1 chops, 2 bass, 3 melody, 4 pads, 10 drums (MPC drum program notes A01-A08, voice order KICK .. PERC)
    static const int kChChops = 0, kChBass = 1, kChMel = 2, kChPads = 3, kChDrums = 9;
    // channel 10: the notes of pads A01-A08 of a new MPC drum program (MPC default map), voice order KICK .. PERC
    static constexpr int kKitNote[hh::DRUMS] = {37, 36, 42, 82, 40, 38, 46, 44};
    struct MidiEv { double at; uint8_t ch, note, vel; bool on; };

    void queueMidi(double on, double off, int ch, int note, float vel)
    {
        if (!midi_.ok() || note < 0 || note > 127 || nMidi_ + 2 > (int)(sizeof(midiQ_) / sizeof(midiQ_[0]))) return;
        uint8_t v = (uint8_t)clamp((int)std::lround(vel * 127.f), 1, 127);
        midiQ_[nMidi_++] = {on, (uint8_t)ch, (uint8_t)note, v, true};
        midiQ_[nMidi_++] = {std::max(on + 1.0, off), (uint8_t)ch, (uint8_t)note, 0, false};
    }

    void scheduleMidi(const Seq& q, int s, uint32_t round, double at, double spStep)
    {
        if (!midi_.ok()) return;
        const double sw = (s & 1) ? (q.swing - 0.5f) * 2.f * spStep : 0.0;
        auto plays = [&](float chance, uint8_t cond, uint32_t seed) {
            if (cond == 1 && (round & 1u)) return false;
            return chance >= 1.f || hh::hash01((uint32_t)s, round, seed) < chance;
        };
        const hh::Note& b = q.bass[s];
        if (b.pitch >= 0 && plays(b.chance, b.cond, 101u))
        {
            double t = at + sw + std::max(0.f, b.off) * spStep;
            queueMidi(t, t + q.bassLen[s] * spStep, kChBass, b.pitch, std::min(1.f, b.vel * (b.acc ? 1.f : 0.85f)));
        }
        const hh::Note& m = q.mel[s];
        if (m.pitch >= 0 && plays(m.chance, m.cond, 102u))
        {
            double t = at + sw + std::max(0.f, m.off) * spStep;
            queueMidi(t, t + q.melLen[s] * spStep, kChMel, m.pitch, std::min(1.f, m.vel * (m.acc ? 1.f : 0.85f)));
        }
        const hh::PadEv& p = q.pad[s];
        if (p.pitch[0] >= 0 && plays(p.chance, p.cond, 103u))
        {
            double t = at + sw + std::max(0.f, p.off) * spStep;
            for (int v = 0; v < hh::PAD_VOICES; ++v)
                if (p.pitch[v] >= 0) queueMidi(t, t + std::max(0.5f, p.len) * spStep * 0.98, kChPads, p.pitch[v], p.vel);
        }
        const bool drumsOff = drumsMuted_.load(std::memory_order_relaxed); // SCHLAG in TRACK: its track plays the drums
        for (int d = 0; d < hh::DRUMS && !drumsOff; ++d)
        {
            const hh::Hit& h = q.drum[d][s];
            if (h.vel <= 0.f || !plays(h.chance, h.cond, 110u + (uint32_t)d)) continue;
            double t = at + sw + std::max(0.f, h.off) * spStep;
            int r = std::max(1, (int)h.ratchet);
            for (int k = 0; k < r; ++k)
            {
                double tk = t + k * spStep / r;
                queueMidi(tk, tk + 0.45 * spStep / r, kChDrums, kKitNote[d], h.vel * (k ? 0.75f : 1.f));
            }
        }
    }

    // send what is due in this block (note-offs first), keep the rest for the next blocks
    void flushMidi(int frames)
    {
        for (int pass = 0; pass < 2; ++pass)
            for (int k = 0; k < nMidi_;)
            {
                MidiEv& e = midiQ_[k];
                if (e.at < (double)frames && e.on == (pass == 1))
                {
                    if (e.on) midi_.noteOn(e.note, e.vel, e.ch);
                    else midi_.noteOff(e.note, e.ch);
                    midiQ_[k] = midiQ_[--nMidi_];
                }
                else ++k;
            }
        for (int k = 0; k < nMidi_; ++k) midiQ_[k].at -= frames;
    }

    void startVoice(int chop, float vel, int pitch, bool rev, double gate, bool fromSeq)
    {
        if (!cur_ || chop < 0 || chop >= cur_->info.count) return;
        lastPlayed_.store(chop, std::memory_order_relaxed);
        const bool choke = getBool(P_CHOKE);
        for (Voice& v : voices_)
            if (v.active && v.release < 0 && (choke || v.chop == chop)) { v.release = kChokeFade; v.relLen = kChokeFade; }
        Voice* slot = nullptr;
        for (Voice& v : voices_) if (!v.active) { slot = &v; break; }
        if (!slot)
        {
            slot = &voices_[0];
            for (Voice& v : voices_) if (v.age > slot->age) slot = &v;
        }
        const float semis = (float)pitch + cPitch_[chop].load(std::memory_order_relaxed);
        Voice& v = *slot;
        v.active = true;
        v.seq = fromSeq;
        v.rev = rev != cRev_[chop].load(std::memory_order_relaxed);
        v.chop = chop;
        v.a = (double)cur_->start[chop];
        v.b = (double)cur_->end[chop];
        v.pos = v.rev ? v.b - 2.0 : v.a;
        v.rate = (double)cur_->data.sampleRate / sr_ * std::pow(2.0, semis / 12.0);
        v.gate = gate;
        float d = cDecay_[chop].load(std::memory_order_relaxed);
        v.decayLen = d >= 99.5f ? 0.0 : std::max(0.03, d / 100.0) * (v.b - v.a) / v.rate;
        v.gain = 0.6f * (0.2f + 0.8f * vel) * std::pow(10.f, cLevel_[chop].load(std::memory_order_relaxed) / 20.f);
        float f = cFilter_[chop].load(std::memory_order_relaxed);
        v.fa = f >= 99.5f ? 1.f : 1.f - std::exp(-2.f * 3.14159265f * cutoffHz(f) / (float)sr_);
        v.zl1 = v.zl2 = v.zr1 = v.zr2 = 0.f;
        v.age = 0;
        v.release = -1;
        v.relLen = kFadeOut;
        if (genralink::LinkData* L = link_.data())
        {
            L->lastChop.store(chop, std::memory_order_relaxed);
            L->trigCount.fetch_add(1, std::memory_order_release);
        }
    }

    void renderVoices(float& l, float& r)
    {
        const std::vector<float>& dl = cur_->data.L;
        const std::vector<float>& dr = cur_->data.R.empty() ? cur_->data.L : cur_->data.R;
        const double n = (double)dl.size();
        for (Voice& v : voices_)
        {
            if (!v.active) continue;
            bool done = v.rev ? v.pos <= v.a + 1.0 : v.pos >= v.b - 1.0;
            size_t k = (size_t)v.pos;
            if (done || v.pos < 0.0 || v.pos + 1.0 >= n) { v.active = false; continue; }
            float f = (float)(v.pos - (double)k);
            float a = dl[k] + (dl[k + 1] - dl[k]) * f;
            float b = dr[k] + (dr[k + 1] - dr[k]) * f;
            if (v.fa < 1.f)
            {
                v.zl1 += v.fa * (a - v.zl1); v.zl2 += v.fa * (v.zl1 - v.zl2); a = v.zl2;
                v.zr1 += v.fa * (b - v.zr1); v.zr2 += v.fa * (v.zr1 - v.zr2); b = v.zr2;
            }
            float env = v.gain;
            if (v.age < kFadeIn) env *= (float)v.age / kFadeIn;
            double left = (v.rev ? v.pos - v.a : v.b - v.pos) / v.rate;
            if (left < kFadeOut) env *= (float)(left / kFadeOut);
            if (v.decayLen > 0.0) env *= (float)std::max(0.0, 1.0 - v.age / v.decayLen);
            if (v.gate >= 0.0 && v.release < 0 && v.age >= v.gate) v.release = kFadeOut;
            if (v.release >= 0)
            {
                env *= (float)v.release / (float)v.relLen;
                if (--v.release <= 0) { v.active = false; continue; }
            }
            l += a * env; r += b * env;
            v.pos += v.rev ? -v.rate : v.rate;
            ++v.age;
        }
    }


    void swapSeq()
    {
        if (Seq* n = seqPending_.exchange(nullptr, std::memory_order_acq_rel))
        {
            if (seq_) retire(seq_, seqRetired_);
            seq_ = n;
        }
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

    // ------------------------------------------------------------------ worker: browser and preview
    void tapRow(int row)
    {
        std::string path = browser_.tap(row);
        rememberDir();
        if (path.empty()) { stopPreview(); return; }
        if (path == previewPath_)
        {
            stopPreview();
            if (loadPath(path)) { cutAndPublish(true); screen_.message(P_LISTINFO, "loaded: " + stripWav(baseName(path))); }
        }
        else startPreview(path);
    }

    void startPreview(const std::string& path)
    {
        std::vector<uint8_t> bytes;
        hh::SampleData* d = new hh::SampleData();
        std::string err;
        if (!mpc::readFile(path, bytes) || !hh::readWav(bytes, *d, err, 30.f))
        {
            delete d;
            screen_.message(P_LISTINFO, "cannot read: " + stripWav(baseName(path)));
            return;
        }
        for (auto& r : previewRetired_) delete r.exchange(nullptr);
        delete previewPending_.exchange(d, std::memory_order_acq_rel);
        previewPath_ = path;
        screen_.message(P_LISTINFO, "tap again = load");
    }

    void stopPreview()
    {
        if (previewPath_.empty()) return;
        previewPath_.clear();
        previewStop_.store(true);
    }

    void rememberDir()
    {
        if (browser_.cwd() != "*" && browser_.cwd() != lastDir_) { lastDir_ = browser_.cwd(); settings_.set("lastdir", lastDir_); settings_.save(); }
    }

    void browserTexts()
    {
        std::string loaded;
        {
            std::lock_guard<std::mutex> g(mtx_);
            loaded = loadedPath_;
        }
        for (int r = 0; r < kRows; ++r) screen_.setText(P_ROWT0 + r, browser_.rowText(r, loaded, previewPath_));
        screen_.setText(P_PATH, browser_.pathText(), true);
        screen_.setText(P_LISTINFO, browser_.info());
    }

    // ------------------------------------------------------------------ worker: sample and cut
    bool loadPath(const std::string& path)
    {
        screen_.setText(P_ANALYSE, "loading ...");
        std::vector<uint8_t> bytes;
        if (!mpc::readFile(path, bytes)) { screen_.setText(P_ANALYSE, "Error: cannot read"); screen_.message(P_STATUS, "cannot read the sample"); return false; }
        hh::SampleData d;
        std::string err;
        if (!hh::readWav(bytes, d, err, 60.f)) { screen_.setText(P_ANALYSE, "Error: " + err); screen_.message(P_STATUS, "Error: " + err); return false; }
        d.path = path;
        d.name = stripWav(baseName(path));
        src_ = std::move(d);
        std::lock_guard<std::mutex> g(mtx_);
        loadedPath_ = path;
        return true;
    }

    void cutAndPublish(bool newSample)
    {
        if (src_.frames() == 0) return;
        auto t0 = std::chrono::steady_clock::now();
        hh::ChopSet* cs = new hh::ChopSet();
        cs->data = src_;
        hh::analyse(*cs, getInt(P_MODE) == 1 ? hh::CM_EQUAL : hh::CM_TRANSIENT, clamp(getInt(P_CHOPS), 4, 64));
        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        info_ = cs->info;
        for (int p = 0; p < hh::PADS; ++p)
        {
            chopStart_[p] = p < cs->info.count ? cs->start[p] : 0;
            chopEnd_[p] = p < cs->info.count ? cs->end[p] : 0;
        }
        computePeaks();
        for (auto& r : retired_) delete r.exchange(nullptr);
        delete pending_.exchange(cs, std::memory_order_acq_rel);
        char buf[64];
        std::snprintf(buf, sizeof buf, "OK, %d Chops in %.2f s", info_.count, secs);
        screen_.setText(P_ANALYSE, buf);
        sampleName_ = src_.name;

        bool restore = false;
        {
            std::lock_guard<std::mutex> g(mtx_);
            if (restored_)
            {
                restore = true;
                restored_ = false;
                pat_ = restoredPattern_;
                rawValid_ = false; // GRID: the loaded chops are the start (already in their grid)
                lastGrid_ = -1;
                for (int c = 0; c < hh::PADS; ++c) padSet_[c].lock = restoredLock_[c];
            }
        }
        if (newSample && !restore)
            for (int c = 0; c < hh::PADS; ++c)
            {
                cPitch_[c].store(0.f); cDecay_[c].store(100.f); cFilter_[c].store(100.f); cLevel_[c].store(0.f);
                cRev_[c].store(false);
                padSet_[c] = hh::PadSet();
            }
        sel_ = std::min(std::max(0, sel_), std::max(0, info_.count - 1));
        knobsFromChop();

        if (genralink::LinkData* L = link_.data())
        {
            L->keyRoot.store(info_.key.root);
            L->keyMinor.store(info_.key.minor ? 1 : 0);
            L->chopCount.store(info_.count);
            for (int p = 0; p < genralink::kChops; ++p)
            {
                hh::Chord c = hh::padChord(info_, padSet_, p);
                L->chordRoot[p].store((int8_t)(p < info_.count && c.valid() ? c.root : -1));
                L->chordQuality[p].store((int8_t)(p < info_.count && c.valid() ? c.quality : -1));
            }
        }
        if (restore) { publishLoop(); screen_.message(P_STATUS, "Project loaded"); }
        else
        {
            regenerate(-1, false);
            screen_.message(P_STATUS, newSample ? "New sample: new loop" : "Chopped again: new loop");
        }
    }

    // ------------------------------------------------------------------ worker: the loop
    float bpmNow() const
    {
        int h = hostBpmX100_.load();
        return h > 0 ? h / 100.f : hh::STYLES[clamp(getInt(P_STYLE), 0, hh::NUM_STYLES - 1)].bpm;
    }

    // GRID (0.13): AUTO and 1/16 = the style's chops as rolled. 1/4 and 1/8: calmer: per quarter / eighth one chop
    // (the strongest of that cell) on the cell's start, by chance a second one half a cell later; no rolls (ratchets),
    // no timing offsets. Acts on the running pattern (from its end, like an edit); back to AUTO = the chops as rolled.
    void applyChopGrid(hh::Pattern& p) const
    {
        const int g = getInt(P_CHOPGRID);
        if (g != 1 && g != 2) return;
        const int cell = g == 1 ? 4 : 2;
        hh::ChopEv out[hh::LEN];
        for (int c0 = 0; c0 < hh::LEN; c0 += cell)
        {
            int a = -1, b = -1; // the strongest and the second strongest hit in the cell
            for (int s = c0; s < c0 + cell; ++s)
            {
                const hh::ChopEv& e = p.chop[s];
                if (e.pad < 0) continue;
                if (a < 0 || e.vel > p.chop[a].vel) { b = a; a = s; }
                else if (b < 0 || e.vel > p.chop[b].vel) b = s;
            }
            if (a < 0) continue;
            uint32_t h = chopSeed_ ^ (uint32_t)(c0 * 2654435761u);
            h ^= h >> 15; h *= 2246822519u; h ^= h >> 13;
            const bool two = b >= 0 && (h & 1u);
            out[c0] = p.chop[a];
            out[c0].off = 0.f;
            out[c0].ratchet = 1;
            out[c0].len = (float)(two ? cell / 2 : cell);
            if (two)
            {
                // the second keeps its own order in time: the earlier of the two hits plays first
                if (b < a) { out[c0] = p.chop[b]; out[c0].off = 0.f; out[c0].ratchet = 1; out[c0].len = (float)(cell / 2); }
                hh::ChopEv& t = out[c0 + cell / 2];
                t = p.chop[b < a ? a : b];
                t.off = 0.f;
                t.ratchet = 1;
                t.len = (float)(cell / 2);
            }
        }
        for (int s = 0; s < hh::LEN; ++s) p.chop[s] = out[s];
    }

    // GRID turned: the running pattern's chops again from the last roll, in the new grid (from the pattern's end)
    void gridChanged()
    {
        if (!rawValid_) { for (int s = 0; s < hh::LEN; ++s) chopRaw_[s] = pat_.chop[s]; rawValid_ = true; }
        for (int s = 0; s < hh::LEN; ++s) pat_.chop[s] = chopRaw_[s];
        applyChopGrid(pat_);
        publishLoop();
        static const char* names[4] = {"AUTO", "1/4", "1/8", "1/16"};
        screen_.message(P_STATUS, std::string("GRID ") + names[clamp(getInt(P_CHOPGRID), 0, 3)] + ": at the pattern end", 2500);
    }

    void regenerate(int track, bool keepUndo = true)
    {
        if (keepUndo)
        {
            if (track < 0) { undoPat_ = pat_; hasUndo_ = true; }
            else if (track < kLanes) { laneUndo_[track] = pat_; laneHasUndo_[track] = true; }
            else { undoPat_ = pat_; hasUndo_ = true; } // NEW CHOPS: UNDO brings the chops back
        }
        set_.style = clamp(getInt(P_STYLE), 0, hh::NUM_STYLES - 1);
        set_.drumMode = clamp(getInt(P_LANEMODE0 + 0), 0, hh::NUM_GROOVES - 1);
        set_.chopPat = clamp(getInt(P_CHOPPAT), 0, hh::NUM_CHOP_PATS - 1);
        set_.bassMode = clamp(getInt(P_LANEMODE0 + 1), 0, 3);
        set_.melMode = clamp(getInt(P_LANEMODE0 + 2), 0, 3);
        set_.padMode = clamp(getInt(P_LANEMODE0 + 3), 0, 2);
        // DENSITY: what NEW takes along (a lane's NEW only its own lane's)
        for (int k = 0; k < kLanes; ++k)
            if (track < 0 || track == k || (track == hh::T_HARMONY && k > 0))
                knobs_.dens[k] = clamp(get(P_DENS0 + k), 0.f, 100.f) / 100.f;
        set_.key = info_.key;
        set_.keyAuto = true;
        seed_ = seed_ * 1664525u + 1013904223u;
        if (track < 0) for (bool& e : pat_.edited) e = false; // NEW ALL: every lane new, hand-edited ones too
        if (track == hh::T_HARMONY) // 0.15 (REC off): the accompaniment to the recorded chops; chops stay as played
        {
            for (int t : {hh::T_BASS, hh::T_MEL, hh::T_PAD}) pat_.edited[t] = false;
            bool drums = false;
            for (int v = 0; v < hh::DRUMS && !drums; ++v)
                for (int st = 0; st < hh::LEN; ++st) if (pat_.drum[v][st].vel > 0.f) { drums = true; break; }
            if (!drums) hh::generate(pat_, hh::T_DRUMS, set_, info_, padSet_, seed_ ^ 0x5eedu, true, bpmNow()); // after CLEAR
        }
        hh::generate(pat_, track, set_, info_, padSet_, seed_, track >= 0 && track != hh::T_HARMONY, bpmNow());
        if (track < 0 || track == hh::T_CHOP)
        {
            for (int s = 0; s < hh::LEN; ++s) chopRaw_[s] = pat_.chop[s]; // the chops as rolled (GRID starts from them)
            rawValid_ = true;
            chopSeed_ = seed_;
            applyChopGrid(pat_);
        }
        if (track < 0) ++loopId_;
        publishLoop();
    }

    // UNDO of one lane (a companion's UNDO): that track comes back, the rest stays
    void undoLane(int k)
    {
        if (!laneHasUndo_[k]) return;
        hh::Pattern& u = laneUndo_[k];
        if (k == 0) for (int v = 0; v < hh::DRUMS; ++v) for (int s = 0; s < kSteps; ++s) std::swap(pat_.drum[v][s], u.drum[v][s]);
        if (k == 1) for (int s = 0; s < kSteps; ++s) std::swap(pat_.bass[s], u.bass[s]);
        if (k == 2) for (int s = 0; s < kSteps; ++s) std::swap(pat_.mel[s], u.mel[s]);
        if (k == 3) for (int s = 0; s < kSteps; ++s) std::swap(pat_.pad[s], u.pad[s]);
        publishLoop();
    }

    void publishLoop(bool edit = false)
    {
        hh::render(pat_, knobs_, rend_);
        {
            std::lock_guard<std::mutex> g(mtx_);
            savedPattern_ = pat_;
        }
        Seq* s = new Seq();
        for (int i = 0; i < kSteps; ++i)
        {
            s->ev[i] = rend_.chop[i];
            s->bass[i] = rend_.bass[i];
            s->mel[i] = rend_.mel[i];
            s->pad[i] = rend_.pad[i];
            for (int d = 0; d < hh::DRUMS; ++d) s->drum[d][i] = rend_.drum[d][i];
        }
        // MIDI lengths: a note that slides into the next one overlaps it (legato), so the MPC glides too
        for (int i = 0; i < kSteps; ++i)
        {
            for (int lane = 0; lane < 2; ++lane)
            {
                const hh::Note* line = lane ? s->mel : s->bass;
                float* len = lane ? s->melLen : s->bassLen;
                if (line[i].pitch < 0) { len[i] = 0.f; continue; }
                len[i] = std::max(0.2f, line[i].len) * 0.95f;
                for (int k = i + 1; k < kSteps && k <= i + 16; ++k)
                    if (line[k].pitch >= 0) { if (line[k].slide) len[i] = std::max(len[i], (float)(k - i) + 0.25f); break; }
            }
        }
        s->swing = hh::STYLES[clamp(set_.style, 0, hh::NUM_STYLES - 1)].swing;
        for (auto& r : seqRetired_) delete r.exchange(nullptr);
        delete seqPending_.exchange(s, std::memory_order_acq_rel);

        genralink::Loop l;
        genralink::Link::clearLoop(l);
        auto note = [](const hh::Note& n) {
            genralink::StepNote o;
            o.pitch = n.pitch;
            o.len16 = (uint8_t)clamp((int)std::lround(n.len * 16.f), 1, 255);
            o.vel = (uint8_t)clamp((int)std::lround(n.vel * 127.f), 1, 127);
            o.flags = (uint8_t)((n.acc ? 1 : 0) | (n.slide ? 2 : 0));
            o.prio = (uint8_t)clamp((int)std::lround(n.prio * 255.f), 0, 255);
            o.off = (int8_t)clamp((int)std::lround(n.off * 100.f), -50, 50);
            return o;
        };
        for (int i = 0; i < kSteps; ++i)
        {
            l.bass[i] = note(pat_.bass[i]); // the base (the base line), with BASS OCT
            if (l.bass[i].pitch >= 0) l.bass[i].pitch = (int8_t)clamp(l.bass[i].pitch + 12 * knobs_.bassOct, 0, 127);
            if (rend_.bass[i].pitch < 0 && pat_.bass[i].pitch >= 0) l.bass[i].prio = 255;
            l.mel[i] = note(rend_.mel[i]);
            for (int v = 0; v < 4; ++v) l.pad[i][v] = rend_.pad[i].pitch[v];
            l.padLen[i] = (uint8_t)clamp((int)std::lround(rend_.pad[i].len), 1, 64);
            for (int v = 0; v < hh::DRUMS; ++v)
            {
                const hh::Hit& h = rend_.drum[v][i];
                genralink::DrumHit& d = l.drum[v][i];
                d.vel = (uint8_t)clamp((int)std::lround(h.vel * 127.f), 0, 127);
                d.off = (int8_t)clamp((int)std::lround(h.off * 100.f), -50, 50);
                d.ratchet = (uint8_t)clamp((int)h.ratchet, 1, 8);
                d.chance = (uint8_t)clamp((int)std::lround(h.chance * 255.f), 0, 255);
                d.cond = h.cond;
                d.prio = (uint8_t)clamp((int)std::lround(h.prio * 255.f), 0, 255);
            }
            l.chop[i] = rend_.chop[i].pad >= 0 ? 1 : 0;
        }
        for (int b = 0; b < genralink::kBars; ++b) { l.barRoot[b] = (int8_t)pat_.bar[b].root; l.barQuality[b] = (int8_t)pat_.bar[b].quality; }
        l.swingX1000 = (uint16_t)std::lround(s->swing * 1000.f);
        l.style = (int16_t)set_.style;
        l.id = loopId_;
        // a hand edit (SCHLAG's STEPS) plays at once, here and in SCHLAG; a new loop waits for the end of the pattern
        if (genralink::LinkData* L = link_.data()) L->loopEditOf.store(edit ? L->loopSeq.load() + 2u : 0u);
        if (edit) swapNow_.store(true);
        link_.writeLoop(l);
    }

    // ------------------------------------------------------------------ worker: the selected chop
    void knobsFromChop()
    {
        int c = clamp(sel_, 0, hh::PADS - 1);
        shownKnob_[0] = cPitch_[c].load(); shownKnob_[1] = cDecay_[c].load();
        shownKnob_[2] = cFilter_[c].load(); shownKnob_[3] = cLevel_[c].load();
        for (int k = 0; k < 4; ++k) { set(P_PITCH + k, shownKnob_[k]); notifyHostParameterChanged(P_PITCH + k); }
        knobSel_ = c;
    }

    void knobsToChop()
    {
        int c = knobSel_;
        if (c < 0 || c >= hh::PADS) return;
        std::atomic<float>* arr[4] = {cPitch_, cDecay_, cFilter_, cLevel_};
        for (int k = 0; k < 4; ++k)
        {
            float v = get(P_PITCH + k);
            if (std::fabs(v - shownKnob_[k]) > 1e-4f) { shownKnob_[k] = v; arr[k][c].store(v); }
        }
    }

    // ------------------------------------------------------------------ worker: the display
    void computePeaks()
    {
        const size_t n = src_.frames();
        const int parts = 1024;
        peaks_.assign(parts, 0.f);
        if (!n) return;
        float mx = 1e-6f;
        for (int p = 0; p < parts; ++p)
        {
            size_t a = n * (size_t)p / parts, b = std::max(a + 1, n * (size_t)(p + 1) / parts);
            float m = 0.f;
            for (size_t i = a; i < b && i < n; i += 4)
            {
                float v = std::fabs(src_.L[i]);
                if (!src_.R.empty()) v = std::max(v, std::fabs(src_.R[i]));
                m = std::max(m, v);
            }
            peaks_[(size_t)p] = m;
            mx = std::max(mx, m);
        }
        for (float& v : peaks_) v /= mx;
    }

    float peakAt(size_t frame) const
    {
        size_t n = src_.frames();
        if (!n || peaks_.empty()) return 0.f;
        size_t p = std::min(peaks_.size() - 1, frame * peaks_.size() / n);
        return peaks_[p];
    }

    static int heightOf(float v) { return clamp((int)std::lround(std::sqrt(std::max(0.f, v)) * (kColH - 1)), 0, kColH - 1); }

    void displayColumns()
    {
        const int view = getInt(P_VIEW);
        const size_t n = src_.frames();
        const int count = info_.count;
        if (!n || !count) { for (int c = 0; c < kCols; ++c) screen_.setFrame(P_COL0 + c, 0); return; }
        if (view == 2)
        {
            int s = clamp(sel_, 0, count - 1);
            size_t a = chopStart_[s], b = std::max(a + 1, chopEnd_[s]);
            for (int c = 0; c < kCols; ++c)
            {
                size_t f = a + (b - a) * (size_t)c / kCols;
                screen_.setFrame(P_COL0 + c, (c == 0 ? 1 : 2) * kColH + heightOf(peakAt(f)));
            }
            return;
        }
        if (view == 1)
        {
            const double stepFrames = src_.sampleRate * 60.0 / bpmNow() / 4.0;
            int curChop = -1, curStart = 0, lastStepSeen = -1;
            float curLen = 1.f;
            for (int c = 0; c < kCols; ++c)
            {
                double pos = (double)c * kSteps / kCols;
                int s = (int)pos;
                bool starts = false;
                for (int k = lastStepSeen + 1; k <= s; ++k)
                    if (rend_.chop[k].pad >= 0 && rend_.chop[k].pad < count)
                    {
                        curChop = rend_.chop[k].pad; curStart = k; curLen = std::max(0.25f, rend_.chop[k].len); starts = true;
                    }
                lastStepSeen = s;
                int h = 0, state = 0;
                if (curChop >= 0 && pos - curStart < curLen)
                {
                    size_t f = chopStart_[curChop] + (size_t)((pos - curStart) * stepFrames);
                    if (f < chopEnd_[curChop]) h = heightOf(peakAt(f));
                    state = starts ? 1 : (curChop == sel_ ? 2 : 0);
                }
                screen_.setFrame(P_COL0 + c, state * kColH + h);
            }
            return;
        }
        int chopOfCol = 0;
        for (int c = 0; c < kCols; ++c)
        {
            size_t a = n * (size_t)c / kCols, b = n * (size_t)(c + 1) / kCols;
            float m = 0.f;
            for (size_t f = a; f < b; f += std::max<size_t>(1, (b - a) / 8)) m = std::max(m, peakAt(f));
            bool marker = false;
            for (int k = 0; k < count; ++k) if (chopStart_[k] >= a && chopStart_[k] < b) { marker = true; chopOfCol = k; }
            while (chopOfCol + 1 < count && chopStart_[chopOfCol + 1] < a) ++chopOfCol;
            int state = marker ? 1 : (chopOfCol == sel_ ? 2 : 0);
            screen_.setFrame(P_COL0 + c, state * kColH + heightOf(m));
        }
    }

    std::string chordText(int chop) const
    {
        if (chop < 0 || chop >= info_.count) return std::string();
        hh::Chord ch = hh::padChord(info_, padSet_, chop);
        if (!ch.valid()) return "HIT";
        char c[16];
        hh::chordName(ch, c, sizeof c);
        return c;
    }

    std::string keyText() const
    {
        char key[16];
        hh::keyName(info_.key, key, sizeof key);
        return key;
    }

    void chopTexts()
    {
        const int bank = clamp(getInt(P_BANK), 0, 3);
        const uint64_t playing = playingMask_.load();
        const float bpm = bpmNow();
        for (int k = 0; k < kPads; ++k)
        {
            int chop = k + kPads * bank;
            bool has = chop < info_.count;
            screen_.setText(P_SLOTTXT0 + k, has ? std::to_string(chop + 1) + " · " + chordText(chop) : std::string());
            std::string tags;
            if (has && cRev_[chop].load()) tags += " R";
            if (has && padSet_[chop].lock) tags += " L";
            screen_.setText(P_SLOTLEN0 + k, has ? lengthText(info_.chop[chop].seconds, bpm) + tags : std::string());
            screen_.setFrame(P_SLOTLIT0 + k, ((playing >> chop) & 1u) ? 2 : (chop == sel_ ? 1 : 0));
        }
        char buf[64];
        std::snprintf(buf, sizeof buf, "%.0f BPM · %s · LINK %d", (double)bpm, keyText().c_str(), linked_);
        screen_.setText(P_HDR, buf);
        screen_.setText(P_LOADED, sampleName_.empty() ? std::string("Test loop (built in)") : sampleName_);
        screen_.setText(P_CHOPSTXT, std::to_string(clamp(getInt(P_CHOPS), 4, 64)));
        int s = clamp(sel_, 0, std::max(0, info_.count - 1));
        if (info_.count > 0)
        {
            std::string st = "CHOP " + std::to_string(s + 1) + " · " + chordText(s) + " · " + lengthText(info_.chop[s].seconds, bpm);
            if (cRev_[s].load()) st += " · REV";
            if (padSet_[s].lock) st += " · LOCK";
            screen_.setText(P_STATUS, st);
            screen_.setFrame(P_REVLIT, cRev_[s].load() ? 1 : 0);
            screen_.setFrame(P_LOCKLIT, padSet_[s].lock ? 1 : 0);
        }
        else screen_.setText(P_STATUS, "no sample");
        int rs = recState_.load();
        if (rs == 1) screen_.setText(P_STATUS, "REC armed: press Play");
        else if (rs == 2) screen_.setText(P_STATUS, "REC: count-in ...");
        else if (rs == 3) screen_.setText(P_STATUS, "REC: play your chops (UNDO = pass)");

        int step = curStep_.load();
        int bar = step >= 0 ? step / hh::STEPS : 0;
        int bars = std::max(1, (loopLenShown_.load() + hh::STEPS - 1) / hh::STEPS);
        screen_.setText(P_BARTXT, step >= 0 ? "Bar " + std::to_string(bar + 1) + " / " + std::to_string(std::min(bars, 4)) : std::string("Stop"));
        for (int j = 0; j < kPads; ++j)
        {
            int st = bar * hh::STEPS + j;
            bool now = step >= 0 && step % hh::STEPS == j;
            screen_.setFrame(P_STEPLIT0 + j, now ? 1 : 0);
            bool has[kLanes] = {false, rend_.bass[st].pitch >= 0, rend_.mel[st].pitch >= 0, rend_.pad[st].pitch[0] >= 0};
            for (int v = 0; v < hh::DRUMS; ++v) if (rend_.drum[v][st].vel > 0.f) has[0] = true;
            for (int k = 0; k < kLanes; ++k) screen_.setFrame(P_CELL0 + k * kPads + j, (has[k] ? 1 : 0) + (now ? 2 : 0));
        }
    }

    // what a lane plays: drums their hits (and SCHLAG if linked), the others their notes and the bars' chords
    std::string laneSummary(int k, bool linked) const
    {
        int n = 0;
        for (int s = 0; s < kSteps; ++s)
        {
            if (k == 0) { for (int v = 0; v < hh::DRUMS; ++v) if (rend_.drum[v][s].vel > 0.f) ++n; }
            else if (k == 1) n += rend_.bass[s].pitch >= 0;
            else if (k == 2) n += rend_.mel[s].pitch >= 0;
            else n += rend_.pad[s].pitch[0] >= 0;
        }
        if (k == 0) return std::to_string(n) + " hits" + (linked ? (drumsMuted_.load() ? " · SCHLAG TRACK" : " · SCHLAG") : "");
        std::string t = std::to_string(n) + (k == 3 ? " chords ·" : " notes ·");
        for (int b = 0; b < hh::BARS; ++b)
        {
            char c[16];
            hh::chordName(pat_.bar[b], c, sizeof c);
            t += std::string(" ") + c;
        }
        return t;
    }

    long long nowMs() const
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    void laneTexts()
    {
        genralink::LinkData* L = link_.data();
        int alive = 0;
        for (int k = 0; k < kLanes; ++k)
        {
            bool on = false;
            std::string name = "not there";
            if (L)
            {
                on = laneAlive_[k].check(L->lane[k].beat.load(), nowMs());
                if (k == 0) drumsMuted_.store(on && L->drumsTrack.load() == 1, std::memory_order_relaxed);
                if (on) { name = genralink::Link::readName(L->lane[k]); if (name.empty()) name = "linked"; }
                uint32_t rq = L->lane[k].newRequest.load(), uq = L->lane[k].undoRequest.load();
                if (!laneReqInit_[k]) { laneReq_[k] = rq; laneUndoReq_[k] = uq; laneReqInit_[k] = true; }
                if (rq != laneReq_[k]) { laneReq_[k] = rq; if (on) newLane(k); }
                if (uq != laneUndoReq_[k]) { laneUndoReq_[k] = uq; if (on) { undoLane(k); screen_.message(P_STATUS, "UNDO from track " + std::to_string(k + 1)); } }
            }
            if (on) ++alive;
            screen_.setFrame(P_LAMP0 + k, on ? 1 : 0);
            screen_.setText(P_LANETXT0 + k, laneSummary(k, on));
            if (k == 0)
            {
                int hits = 0;
                for (int st = 0; st < kSteps; ++st) hits += rend_.chop[st].pad >= 0;
                screen_.setText(P_CHOPTXT, std::to_string(info_.count) + " chops · " + std::to_string(hits) + " hits");
            }
        }
        linked_ = alive;
        // LINK and MIDI OUT share the row: "2 linked · OUT 130:0 · 512"
        std::string out = midi_.ok() ? "OUT " + midi_.status().substr(midi_.status().find("OUT ") + 4) + " · " + std::to_string(midi_.sent())
                                     : "MIDI off: " + midi_.status();
        if (!L) screen_.setText(P_LINKINFO, "Error: " + link_.error());
        else screen_.setText(P_LINKINFO, (alive ? std::to_string(alive) + " linked · " : std::string()) + out);
    }

    void newLane(int k)
    {
        const char* names[kLanes] = {"Drums", "Bass", "Melody", "Pads"};
        regenerate(k);
        screen_.message(P_STATUS, std::string("NEW ") + names[k] + ": at the end of the pattern");
    }

    // ------------------------------------------------------------------ worker: files
    void saveChop()
    {
        int chop = sel_;
        if (chop < 0 || chop >= info_.count || src_.frames() == 0) { screen_.message(P_STATUS, "choose a chop first"); return; }
        std::string dir;
        {
            std::lock_guard<std::mutex> g(mtx_);
            size_t i = loadedPath_.find_last_of('/');
            dir = i == std::string::npos ? genraDir() : loadedPath_.substr(0, i);
        }
        dir += "/GENRA CHOP";
        if (!saveDir_.empty() && isDir(saveDir_)) dir = saveDir_;
        ::mkdir(dir.c_str(), 0777);
        if (chopEnd_[chop] <= chopStart_[chop]) { screen_.message(P_STATUS, "the chop is empty"); return; }
        std::vector<uint8_t> wav = hh::writeWav(src_, chopStart_[chop], chopEnd_[chop], cRev_[chop].load(), (int)std::lround(cPitch_[chop].load()));
        char name[96];
        std::snprintf(name, sizeof name, "/%s chop%02d.wav", sampleName_.substr(0, 60).c_str(), chop + 1);
        bool ok = writeFileBytes(dir + name, wav.data(), wav.size());
        screen_.message(P_STATUS, ok ? "Chop " + std::to_string(chop + 1) + " saved as WAV" : std::string("Error while saving"));
        screen_.setText(P_WRITE, ok ? "OK" : "Error: WAV");
    }

    // PRINT: the chop track as a MIDI file (pad notes as learned; chops above 16 play on the bank chosen)
    void print()
    {
        std::string dir = printDir(printDirChosen_);
        if (dir.empty()) { screen_.message(P_PRINTSTATUS, "no folder to write to"); return; }
        const float bpm = bpmNow();
        const float swing = hh::STYLES[clamp(set_.style, 0, hh::NUM_STYLES - 1)].swing;
        std::vector<MidiNote> notes;
        for (int s = 0; s < kSteps; ++s)
        {
            const hh::ChopEv& e = rend_.chop[s];
            if (e.pad < 0 || e.pad >= info_.count || e.chance < 0.5f) continue;
            double at = s + ((s & 1) ? (swing - 0.5f) * 2.f : 0.f) + std::max(0.f, e.off);
            int r = std::max(1, (int)e.ratchet);
            for (int k = 0; k < r; ++k)
                notes.push_back({at + (double)k / r, r > 1 ? 1.0 / r : std::max(0.25, (double)e.len), pads_.noteOf(e.pad % kPads),
                                 (int)std::lround(e.vel * 127.f * (k ? 0.8f : 1.f))});
        }
        int no = printNumberFor(dir, loopId_);
        char name[96];
        std::snprintf(name, sizeof name, "Print %02d Chop %s %.0f", no, keyText().c_str(), (double)bpm);
        std::vector<uint8_t> f = midiFile(notes, bpm, name, kSteps);
        bool ok = writeFileBytes(dir + "/" + name + ".mid", f.data(), f.size());
        if (ok)
        {
            set(P_AUTO, 1.f);
            notifyHostParameterChanged(P_AUTO);
            screen_.message(P_PRINTSTATUS, std::string(name) + " saved, now PADS", 4000);
            screen_.message(P_STATUS, "PRINT done: now PADS", 3000);
        }
        else screen_.message(P_PRINTSTATUS, "Error while writing");
        printTexts();
    }

    void printTexts()
    {
        std::string dir = printDir(printDirChosen_);
        screen_.setText(P_PRINTDIRTXT, dir.empty() ? std::string("no folder") : pathLabel(dir), true);
        std::vector<std::string> list = dir.empty() ? std::vector<std::string>() : listPrints(dir);
        for (int r = 0; r < kPrintRows; ++r)
            screen_.setText(P_PRINTROW0 + r, r < (int)list.size() ? (r == 0 ? "> " : "") + list[(size_t)r].substr(0, list[(size_t)r].size() - 4) : std::string());
        char b[64];
        std::snprintf(b, sizeof b, "Chop track · 4 bars · %.0f BPM · %s", (double)bpmNow(), keyText().c_str());
        screen_.setText(P_PRINTINFO, b);
        screen_.setText(P_PRINTSTATUS, getInt(P_AUTO) == 0 ? "AUTO: the loop plays" : "PADS: you play the chops, GENRA the rest");
    }

    void setupTexts()
    {
        screen_.setText(P_LEARNINFO, pads_.learning() ? pads_.info() : (pads_.learned() ? "learned" : "not learned yet"));
        screen_.setText(P_MAPINFO, pads_.info());
        screen_.setText(P_SAVEDIRTXT, saveDir_.empty() ? std::string("next to the sample (/GENRA CHOP)") : pathLabel(saveDir_), true);
        std::string names;
        for (const std::string& d : findDrives(false)) names += (names.empty() ? "" : ", ") + pathLabel(d);
        screen_.setText(P_DRIVES, names.empty() ? std::string("none found") : names);
        std::string g = genraDir();
        screen_.setText(P_WRITE, g.empty() ? std::string("no Genra folder") : "Genra folder: " + pathLabel(g));
    }

    // what the MPC tells the plugin about its loop (to learn whether GENRA can follow the sequence length)
    void hostTexts()
    {
        char b[96];
        // DIAG: the capture's state (texts on the MPC: at most 31 characters)
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
        int sig = hostSig_.load();
        double c0 = hostCycle_[0].load(), c1 = hostCycle_[1].load();
        if (diagCount_.load() > 0)
        {
            // 0.12: what the MPC reported at the last start (count-in): q = position, s = sample position (k),
            // r = recording (1 at the start, 2 later), j = a jump back (from > to), f = blocks the position stood still
            const double jf = diagJumpFrom_.load(), jt = diagJumpTo_.load();
            char j[24];
            if (jf >= 0.0) std::snprintf(j, sizeof j, "j%.1f>%.1f", jf, jt);
            else std::snprintf(j, sizeof j, "j-");
            std::snprintf(b, sizeof b, "q%.2f s%.0fk r%d %s f%d", diagQ0_.load(), diagS0_.load() / 1000.0, diagRec_.load(),
                          j, std::min(diagFrozen_.load(), 999));
        }
        else if (hostCycling_.load() || c1 > c0)
            std::snprintf(b, sizeof b, "loop %s %.1f-%.1f beats · %d/%d", hostCycling_.load() ? "on" : "off", c0, c1, sig / 100, sig % 100);
        else
            std::snprintf(b, sizeof b, "no loop info · bar at %.1f · %d/%d", hostBar_.load(), sig / 100, sig % 100);
        screen_.setText(P_HOSTINFO, b);
        int st = clamp(getInt(P_STYLE), 0, hh::NUM_STYLES - 1);
        const hh::Style& y = hh::STYLES[st];
        std::snprintf(b, sizeof b, "%s · %s · %.0f BPM", y.shortName, hh::ERA_NAMES[y.era], y.bpm);
        screen_.setText(P_STYLEHDR, b);
    }

    void setParam(int i, float x) { set(i, x); notifyHostParameterChanged(i); }

    // SCHLAG's STEPS: HIT ON / OFF on a drum step; the drum lane counts as edited (NEW ALL keeps it, NEW DRUMS rolls)
    void takeDrumEdits()
    {
        genralink::LinkData* L = link_.data();
        if (!L) return;
        uint32_t w = L->editW.load(std::memory_order_acquire);
        if (!editRSet_) { editR_ = w; editRSet_ = true; return; } // edits from before this plugin started: not ours
        if (w == editR_) return;
        if (w - editR_ > (uint32_t)genralink::LinkData::kEdits) editR_ = w - genralink::LinkData::kEdits;
        undoPat_ = pat_;
        hasUndo_ = true;
        // the first edit takes the drums as they play (DENSITY applied): hits DENSITY hid stay away, and HIT OFF
        // finds every hit SCHLAG shows
        if (!pat_.edited[hh::T_DRUMS])
            for (int v = 0; v < hh::DRUMS; ++v)
                for (int s = 0; s < kSteps; ++s) pat_.drum[v][s] = rend_.drum[v][s];
        bool cleared = false;
        for (; editR_ != w; ++editR_)
        {
            int c = L->editCmd[editR_ % genralink::LinkData::kEdits].load(std::memory_order_relaxed);
            int v = c & 15, st = (c >> 4) & 255;
            bool on = (c >> 12) & 1;
            if (v == genralink::kEditClearAll)
            {
                for (int d = 0; d < hh::DRUMS; ++d) for (int s = 0; s < kSteps; ++s) pat_.drum[d][s] = hh::Hit();
                cleared = true;
                continue;
            }
            if (v >= hh::DRUMS || st >= kSteps) continue;
            hh::Hit& h = pat_.drum[v][st];
            if (on) { h = hh::Hit(); h.vel = 0.85f; h.prio = 0.f; }
            else h = hh::Hit();
        }
        pat_.edited[hh::T_DRUMS] = true;
        publishLoop(true);
        screen_.message(P_STATUS, cleared ? "SCHLAG: CLEAR ALL (UNDO = back)" : "SCHLAG: drum step edited (NEW DRUMS rolls again)", 3000);
    }

    // REC (worker): the pad hits go into the chop lane, on the nearest 16th, overdubbing pass by pass
    void takeRecorded()
    {
        int r = recR_.load(std::memory_order_relaxed), w = recW_.load(std::memory_order_acquire);
        if (r != w)
        {
            const long len = std::max(4, loopLenShown_.load());
            const float bpm = bpmNow();
            for (; r != w; ++r)
            {
                const RecHit& h = recQ_[r % kRecQ];
                long q = (long)std::llround(h.q) - loopStartShown_.load();
                long pass = (long)std::floor((double)q / len);
                long pos = ((q % len) + len) % len;
                int st = (int)(pos % kSteps);
                if (pass != recPass_) { undoPat_ = pat_; hasUndo_ = true; recPass_ = pass; } // UNDO: this pass
                hh::ChopEv e;
                e.pad = (int8_t)h.chop;
                e.vel = std::max(0.2f, h.vel);
                float secs = h.chop < info_.count ? info_.chop[h.chop].seconds : 0.25f;
                e.len = clamp(secs * bpm / 60.f * 4.f, 0.5f, 16.f);
                pat_.chop[st] = e;
                recDirty_ = true;
            }
            recR_.store(r, std::memory_order_release);
        }
        auto now = std::chrono::steady_clock::now();
        if (recDirty_ && now - recPublish_ > std::chrono::milliseconds(150)) { recDirty_ = false; recPublish_ = now; publishLoop(); }
        if (recState_.load() == 0) recPass_ = -1;
        // 0.15: recording over (REC off or the MPC stopped): GENRA builds bass, melody and pads to the chops at once
        if (recState_.load() == 3) recTook_ = recTook_ || r != recSeen_;
        else if (recTook_)
        {
            recTook_ = false;
            regenerate(hh::T_HARMONY);
            screen_.message(P_STATUS, "REC off: bass, melody, pads to your chops (UNDO = back)", 4000);
        }
        recSeen_ = r;
    }

    // ------------------------------------------------------------------ worker: buttons
    void handleTaps()
    {
        // GRID turned (knob or Q-Link): the running pattern's chops in the new grid, from the pattern's end
        {
            const int g = getInt(P_CHOPGRID);
            if (lastGrid_ < 0) lastGrid_ = g;
            else if (g != lastGrid_) { lastGrid_ = g; gridChanged(); }
        }
        for (auto& t : screen_.taps())
        {
            const int p = t.first;
            if (p == P_NEWALL) { regenerate(-1); screen_.message(P_STATUS, "NEW ALL: new loop at the end of the pattern"); }
            else if (p == P_OCTTAP) setParam(P_BASSOCT, (float)((getInt(P_BASSOCT) + 1) % 4));
            else if (p == P_PATPREV || p == P_PATNEXT)
            {
                const int n = hh::NUM_CHOP_PATS;
                setParam(P_CHOPPAT, (float)((getInt(P_CHOPPAT) + (p == P_PATNEXT ? 1 : n - 1)) % n));
                screen_.message(P_STATUS, std::string("PAT ") + hh::CHOP_PAT_NAMES[clamp(getInt(P_CHOPPAT), 0, n - 1)] + ": with NEW", 2500);
            }
            else if (p == P_NEWCHOPS)
            {
                regenerate(hh::T_CHOP);
                screen_.message(P_STATUS, "NEW CHOPS: at the end of the pattern");
            }
            else if (p >= P_NEW0 && p < P_NEW0 + kLanes) newLane(p - P_NEW0);
            else if (p == P_UNDO)
            {
                if (hasUndo_) { std::swap(pat_, undoPat_); publishLoop(); screen_.message(P_STATUS, "UNDO: the last loop"); }
                else screen_.message(P_STATUS, "nothing to undo");
            }
            else if (p == P_SAVE) saveChop();
            else if (p == P_CHMINUS || p == P_CHPLUS)
            {
                set(P_CHOPS, (float)clamp(getInt(P_CHOPS) + (p == P_CHPLUS ? 1 : -1), 4, 64));
                notifyHostParameterChanged(P_CHOPS);
            }
            else if (p == P_STPREV || p == P_STNEXT)
            {
                int s = (getInt(P_STYLE) + (p == P_STNEXT ? 1 : hh::NUM_STYLES - 1)) % hh::NUM_STYLES;
                set(P_STYLE, (float)s);
                notifyHostParameterChanged(P_STYLE);
            }
            else if (p == P_LOADSMP)
            {
                if (previewPath_.empty()) screen_.message(P_LISTINFO, "tap a WAV first");
                else
                {
                    std::string path = previewPath_;
                    stopPreview();
                    if (loadPath(path)) { cutAndPublish(true); screen_.message(P_LISTINFO, "loaded: " + stripWav(baseName(path))); }
                }
            }
            else if (p == P_CLEAR)
            {
                if (hostPlaying_.load()) screen_.message(P_STATUS, "CLEAR: stop the MPC first");
                else
                {
                    undoPat_ = pat_;
                    hasUndo_ = true;
                    pat_.clear();
                    publishLoop();
                    screen_.message(P_STATUS, "CLEAR: empty. PADS + REC + Play: your chops", 4000);
                }
            }
            else if (p >= P_MODEPREV0 && p < P_MODENEXT0 + kLanes)
            {
                static const int n[kLanes] = {hh::NUM_GROOVES, 4, 4, 3};
                int k = (p - P_MODEPREV0) % kLanes, d = p >= P_MODENEXT0 ? 1 : n[k] - 1;
                setParam(P_LANEMODE0 + k, (float)((getInt(P_LANEMODE0 + k) + d) % n[k]));
                screen_.message(P_STATUS, "MODE: with the next NEW", 2500);
            }
            else if (p == P_REC)
                screen_.message(P_STATUS, getBool(P_REC) ? "REC on: press Play, 1 bar count-in" : "REC off", 3000);
            else if (p == P_REVBTN && sel_ < info_.count) { cRev_[sel_].store(!cRev_[sel_].load()); }
            else if (p == P_LOCKBTN && sel_ < info_.count) { padSet_[sel_].lock = !padSet_[sel_].lock; }
            else if (p == P_PRINT) print();
            else if (p == P_PRINTHERE)
            {
                if (browser_.cwd().empty() || browser_.cwd() == "*") screen_.message(P_PRINTSTATUS, "open a folder in the BROWSER first");
                else { printDirChosen_ = browser_.cwd(); settings_.set("printdir", printDirChosen_); settings_.save(); screen_.message(P_PRINTSTATUS, "Print folder set"); }
            }
            else if (p == P_PRINTSTD) { printDirChosen_.clear(); settings_.set("printdir", ""); settings_.save(); screen_.message(P_PRINTSTATUS, "Print folder: Genra/PRINT"); }
            else if (p == P_LEARN) pads_.startLearning();
            else if (p == P_DIAG)
            {
                const std::string dir = genraDir();
                if (dir.empty()) screen_.message(P_STATUS, "DIAG: no drive to write to", 3000);
                else if (!diag_.running())
                {
                    const std::string head = "GENRA CHOP 0.15 · LINK v3 · MIDI OUT: " + midi_.status();
                    diag_.start(dir, head, &diagTr_, 120);
                    screen_.message(P_STATUS, "DIAG: 2 min, now use the MPC", 4000);
                }
            }
            else if (p == P_SCROLL) browser_.scroll(t.second > 0 ? 1 + (t.second - 1) / 6 : -1 - (-t.second - 1) / 6);
            else if (p == P_BACK) { browser_.up(); rememberDir(); stopPreview(); }
            else if (p == P_DRVBTN) { browser_.drives(); stopPreview(); }
            else if (p == P_PGUP) browser_.page(-1);
            else if (p == P_PGDN) browser_.page(1);
            else if (p == P_SAVEHERE)
            {
                if (browser_.cwd().empty() || browser_.cwd() == "*") screen_.message(P_LISTINFO, "open a folder first");
                else { saveDir_ = browser_.cwd(); settings_.set("savedir", saveDir_); settings_.save(); screen_.message(P_LISTINFO, "Save folder set"); }
            }
            else if (p == P_AUTO) screen_.message(P_STATUS, getInt(P_AUTO) == 0 ? "AUTO: the loop plays" : "PADS: you play the chops, GENRA the rest");
            else if (p >= P_ROWTAP0 && p < P_ROWTAP0 + kRows) tapRow(p - P_ROWTAP0);
        }
    }

    void workerMain()
    {
        using clock = std::chrono::steady_clock;
        lastDir_ = settings_.get("lastdir");
        saveDir_ = settings_.get("savedir");
        printDirChosen_ = settings_.get("printdir");
        browser_.rows = kRows;
        browser_.open(lastDir_);
        clock::time_point cutChanged{}, lastSlow{}, styleChanged{};
        const clock::time_point started = clock::now();
        int lastMode = getInt(P_MODE), lastChops = getInt(P_CHOPS), lastStyle = getInt(P_STYLE);
        bool cutPending = false, stylePending = false;
        while (!quit_.load())
        {
            auto now = clock::now();
            std::string want;
            bool wantLoad = false, wasRestored = false;
            {
                std::lock_guard<std::mutex> g(mtx_);
                if (pathRequest_) { want = requestedPath_; pathRequest_ = false; wantLoad = true; wasRestored = restored_; }
            }
            if (resyncReq_.exchange(false)) screen_.resync();
            {
                int o = clamp(getInt(P_BASSOCT), 0, 3) - 1;
                if (o != knobs_.bassOct)
                {
                    knobs_.bassOct = o;
                    if (octSeen_) { publishLoop(true); screen_.message(P_STATUS, "BASS OCT " + std::string(o > 0 ? "+" : "") + std::to_string(o)); }
                }
                octSeen_ = true;
            }
            if (wantLoad)
            {
                if (!want.empty() && loadPath(want)) cutAndPublish(true);
                else if (wasRestored) cutAndPublish(false);
            }
            handleTaps();
            takeRecorded();
            takeDrumEdits();
            if (bankReset_.exchange(false)) { notifyHostParameterChanged(P_BANK); requestDisplayUpdate(); }
            if (pads_.finishIfDone()) screen_.message(P_LEARNINFO, "learned and saved", 3000);

            int req = selReq_.exchange(-1);
            if (req >= 0 && req < std::max(1, info_.count) && req != sel_) { sel_ = req; knobsFromChop(); }
            else knobsToChop();

            int m = getInt(P_MODE), c = getInt(P_CHOPS);
            if (m != lastMode || c != lastChops) { lastMode = m; lastChops = c; cutPending = true; cutChanged = now; }
            if (cutPending && now - cutChanged > std::chrono::milliseconds(400)) { cutPending = false; cutAndPublish(false); }
            int st = getInt(P_STYLE);
            // a project being loaded sets the style too: that is no reason for a new loop
            if (stateLoaded_.exchange(false) || now - started < std::chrono::seconds(2)) { lastStyle = st; stylePending = false; }
            if (st != lastStyle) { lastStyle = st; stylePending = true; styleChanged = now; }
            // a new style rolls a new loop in it (after the knob / the keys rest for a moment)
            if (stylePending && now - styleChanged > std::chrono::milliseconds(350))
            {
                stylePending = false;
                regenerate(-1);
                screen_.message(P_STATUS, std::string(hh::STYLES[st].shortName) + ": new loop at the end of the pattern", 4000);
            }

            if (now - lastSlow > std::chrono::seconds(2))
            {
                lastSlow = now;
                setupTexts();
                printTexts();
                if (browser_.cwd().empty()) browser_.list();
            }
            else if (pads_.learning()) setupTexts();
            browserTexts();
            chopTexts();
            laneTexts();
            hostTexts();
            displayColumns();
            screen_.push();
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
        }
    }

    // ------------------------------------------------------------------ data
    genraui::Screen screen_;
    // audio thread
    hh::ChopSet* cur_ = nullptr;
    Seq* seq_ = nullptr;
    std::atomic<bool> stateLoaded_{false}, hostCycling_{false}, hostPlaying_{false}, swapNow_{false};
    uint32_t editR_ = 0;
    bool editRSet_ = false;
    long loopLen_ = 64, loopStart_ = 0;           // audio thread
    std::atomic<int> loopLenShown_{64}, recState_{0};
    std::atomic<long> loopStartShown_{0};
    // REC: pad hits from the audio thread to the worker
    struct RecHit { int chop; float vel; double q; };
    static const int kRecQ = 256;
    RecHit recQ_[kRecQ];
    std::atomic<int> recW_{0}, recR_{0};
    double recFrom_ = 0.0;                        // audio thread: REC writes from this step on (after the count-in)
    std::atomic<double> recFromShared_{0.0};
    bool recWas_ = false;
    bool recTook_ = false;                        // worker: hits came in during this recording (0.15)
    int recSeen_ = 0;
    long recPass_ = -1;                           // worker: the pass being recorded (UNDO takes it back)
    std::chrono::steady_clock::time_point recPublish_{};
    bool recDirty_ = false;
    std::atomic<double> hostCycle_[2], hostBar_{0.0};
    std::atomic<int> hostSig_{404};
    hh::SampleData* preview_ = nullptr;
    double previewPos_ = 0.0;
    int previewFade_ = -1;
    Voice voices_[kVoices];
    Trig trig_[48];
    int nTrig_ = 0;
    double sr_ = 44100.0;
    double nextStep_ = 0.0;
    bool wasPlaying_ = false;
    hh::ChopEv chopRaw_[hh::LEN];  // worker: the chop lane as rolled, before GRID
    bool rawValid_ = false;
    int lastGrid_ = -1;
    uint32_t chopSeed_ = 1;        // the roll the chops came from (GRID's chance of a second chop)
    genramidi::Out midi_;
    MidiEv midiQ_[1024];
    int nMidi_ = 0;
    float gainSmooth_ = -1.f;
    int slotTapSeen_[kPads] = {};
    // shared with the audio thread (lock-free)
    std::atomic<hh::ChopSet*> pending_{nullptr};
    std::atomic<hh::ChopSet*> retired_[4];
    std::atomic<Seq*> seqPending_{nullptr};
    std::atomic<Seq*> seqRetired_[8];
    std::atomic<hh::SampleData*> previewPending_{nullptr};
    std::atomic<hh::SampleData*> previewRetired_[4];
    std::atomic<bool> previewStop_{false}, resyncReq_{false}, bankReset_{false};
    std::atomic<int> lastPlayed_{-1}, hostBpmX100_{0}, curStep_{-1}, selReq_{-1};
    std::atomic<uint64_t> playingMask_{0};
    genradiag::Transport diagTr_;  // DIAG: the host transport (audio thread writes)
    genradiag::Diag diag_;         // DIAG: the capture (own thread)
    // count-in measurement (0.12): what the MPC reports at a start (audio thread writes, SETUP shows it)
    bool diagWas_ = false;
    double diagLastQ_ = 0.0;
    int diagBlocks_ = 0;
    std::atomic<double> diagQ0_{0.0}, diagS0_{0.0}, diagJumpFrom_{-1.0}, diagJumpTo_{-1.0};
    std::atomic<int> diagRec_{0}, diagFrozen_{0}, diagCount_{0};
    std::atomic<float> cPitch_[hh::PADS], cDecay_[hh::PADS], cFilter_[hh::PADS], cLevel_[hh::PADS];
    std::atomic<bool> cRev_[hh::PADS];
    PadMap pads_;
    // worker
    std::thread worker_;
    std::atomic<bool> quit_{false};
    bool started_ = false;
    genralink::Link link_;
    Settings settings_{"GENRA CHOP.txt"};
    Browser browser_;
    std::string previewPath_, lastDir_, saveDir_, printDirChosen_;
    hh::SampleData src_;
    hh::SampleInfo info_;
    size_t chopStart_[hh::PADS] = {}, chopEnd_[hh::PADS] = {};
    std::vector<float> peaks_;
    std::string sampleName_;
    hh::Pattern pat_, undoPat_, laneUndo_[kLanes];
    bool hasUndo_ = false, laneHasUndo_[kLanes] = {};
    hh::PadSet padSet_[hh::PADS];
    hh::Settings set_;
    hh::Knobs knobs_;
    hh::Render rend_;
    uint32_t seed_ = 1, loopId_ = 1;
    int sel_ = 0, knobSel_ = -1, linked_ = 0;
    float shownKnob_[4] = {};
    genralink::Link::Alive laneAlive_[kLanes];
    uint32_t laneReq_[kLanes] = {}, laneUndoReq_[kLanes] = {};
    bool laneReqInit_[kLanes] = {};
    // shared between worker, UI thread and state (mtx_)
    std::mutex mtx_;
    std::string loadedPath_, requestedPath_;
    bool pathRequest_ = false, restored_ = false;
    bool restoredLock_[hh::PADS] = {};
    hh::Pattern savedPattern_, restoredPattern_;
    bool octSeen_ = false;                 // BASS OCT: the worker has taken the parameter once
    std::atomic<bool> drumsMuted_{false};  // SCHLAG in TRACK: no drums on MIDI channel 10
};

MPC_PLUGIN_ENTRY(GenraChop)
