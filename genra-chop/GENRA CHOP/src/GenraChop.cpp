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
#include "../../common/genra_ui.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
#include <thread>

using namespace mpc;
using namespace genrafiles;

namespace {

static const int kRows = 10;   // rows of the browser
static const int kPads = 16;   // chop slots per bank
static const int kCols = 124;  // columns of the display
static const int kLanes = 4;
static const int kPrintRows = 7;
static const int kColFrames = 48, kColH = 16;  // column strip: 3 states x 16 heights
static const int kSteps = hh::LEN;             // 64

static const char* const kCaptions[][2] = {
    {"genra", "GENRA"}, {"chop", "CHOP"}, {"sample", "SAMPLE"}, {"loop", "LOOP"}, {"chopv", "CHOP"},
    {"style", "STYLE"}, {"newall", "NEW ALL"}, {"new", "NEW"}, {"modus", "MODUS"}, {"transient", "Transient"},
    {"equal", "Equal"}, {"chops", "CHOPS"}, {"minus", "-"}, {"plus", "+"}, {"bank", "BANK"},
    {"bankA", "A"}, {"bankB", "B"}, {"bankC", "C"}, {"bankD", "D"}, {"undo", "UNDO"}, {"savewav", "SAVE WAV"},
    {"drums", "DRUMS"}, {"bass", "BASS"}, {"mel", "MELODIE"}, {"pads", "PADS"}, {"prev", "<"}, {"next", ">"},
    {"auto", "AUTO"}, {"padsmode", "PADS"}, {"choke", "CHOKE"}, {"rev", "REV"}, {"lock", "LOCK"},
    {"print", "PRINT"}, {"printtitle", "· PRINT"}, {"after", "Nach PRINT"}, {"autoinfo", "AUTO: der Loop spielt"},
    {"padsinfo", "PADS: nur die Noten der MPC-Spur"}, {"printhere", "SPEICHERORT HIER"}, {"printstd", "STANDARD"},
    {"back", "ZURÜCK"}, {"drives", "LAUFWERKE"}, {"pgup", "< SEITE"}, {"pgdn", "SEITE >"},
    {"savehere", "SPEICHERORT HIER"}, {"loaded", "Geladen:"}, {"learn", "LERNEN"},
    {"help1", "LERNEN antippen, dann die"}, {"help2", "Pads 1 bis 16 der Reihe nach"},
    {"help3", "druecken (Pad 1 = unten links)"}, {"help4", "Gilt fuer alle GENRA-Plugins"},
    {"r1", "Pad-Belegung"}, {"r2", "Laufwerke"}, {"r3", "Schreiben"}, {"r4", "LINK"}, {"r5", "Analyse"},
    {"r6", "SAVE WAV nach"}};
static const int kNumCaps = (int)(sizeof(kCaptions) / sizeof(kCaptions[0]));

// the keys that flash when tapped (in this order: FLASH 1.. in the skin)
static const char* const kFlashKeys[] = {"NEW ALL", "NEW DRUMS", "NEW BASS", "NEW MELODIE", "NEW PADS", "UNDO",
    "SAVE WAV", "CHOPS MINUS", "CHOPS PLUS", "STYLE ZURUECK", "STYLE VOR", "PRINT", "LERNEN", "ZURUECK",
    "LAUFWERKE", "SEITE ZURUECK", "SEITE VOR", "SPEICHERORT HIER", "PRINT HIER", "PRINT STANDARD"};
static const int kNumFlash = (int)(sizeof(kFlashKeys) / sizeof(kFlashKeys[0]));

enum
{
    P_PITCH = 0, P_DECAY, P_FILTER, P_LEVEL,
    P_VOLUME, P_STYLE, P_MODE, P_CHOPS, P_BANK, P_VIEW, P_CHOKE, P_AUTO,
    // buttons (every tap flips them); the first kNumFlash have a flash, in kFlashKeys' order
    P_NEWALL, P_NEW0,
    P_UNDO = P_NEW0 + kLanes, P_SAVE, P_CHMINUS, P_CHPLUS, P_STPREV, P_STNEXT, P_PRINT, P_LEARN,
    P_BACK, P_DRVBTN, P_PGUP, P_PGDN, P_SAVEHERE, P_PRINTHERE, P_PRINTSTD,
    P_REVBTN, P_LOCKBTN, P_SCROLL,
    P_ROWTAP0,
    P_SLOTTAP0 = P_ROWTAP0 + kRows,
    // texts
    P_PATH = P_SLOTTAP0 + kPads,
    P_LISTINFO, P_LOADED, P_HDR, P_CHOPSTXT, P_BARTXT, P_STATUS, P_LEARNINFO, P_MAPINFO, P_DRIVES, P_WRITE,
    P_LINKINFO, P_ANALYSE, P_SAVEDIRTXT, P_PRINTDIRTXT, P_PRINTINFO, P_PRINTSTATUS,
    P_SLOTTXT0,
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
static const int kFadeIn = 32;
static const int kFadeOut = 96;

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
};

struct Trig { double at; int chop; float vel; int pitch; bool rev; double gate; };

struct Seq
{
    hh::ChopEv ev[hh::LEN];
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
    s.name = "Testloop (eingebaut)";
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
        addParam(choiceParam("mode", "MODUS", {"Transient", "Equal"}, 0));
        addParam(intParam("chops", "CHOPS", 4, 64, 16));
        addParam(choiceParam("bank", "BANK", {"A", "B", "C", "D"}, 0));
        addParam(choiceParam("view", "ANSICHT", {"Sample", "Loop", "Chop"}, 0));
        addParam(boolParam("choke", "CHOKE", true));
        addParam(choiceParam("auto", "AUTO PADS", {"Auto", "Pads"}, 0));
        const char* lanes[kLanes] = {"DRUMS", "BASS", "MELODIE", "PADS"};
        addButton("NEW ALL");
        for (int k = 0; k < kLanes; ++k) addButton((std::string("NEW ") + lanes[k]).c_str());
        for (const char* b : {"UNDO", "SAVE WAV", "CHOPS MINUS", "CHOPS PLUS", "STYLE ZURUECK", "STYLE VOR", "PRINT",
                              "LERNEN", "ZURUECK", "LAUFWERKE", "SEITE ZURUECK", "SEITE VOR", "SPEICHERORT HIER",
                              "PRINT HIER", "PRINT STANDARD", "REV", "LOCK"})
            addButton(b);
        addParam(intParam("scroll", "LISTE", 0, 1000, 500));
        for (int i = 0; i < kRows; ++i) addButton(("TIPP " + std::to_string(i + 1)).c_str());
        for (int i = 0; i < kPads; ++i) addButton(("SLOT " + std::to_string(i + 1)).c_str());
        for (const char* t : {"PFAD", "LISTINFO", "GELADEN", "KOPF", "CHOPZAHL", "TAKT", "STATUS", "LERNINFO",
                              "BELEGUNG", "LAUFWERKSLISTE", "SCHREIBEN", "LINKINFO", "ANALYSE", "SPEICHERORT",
                              "PRINTORDNER", "PRINTINFO", "PRINTSTATUS"})
            addText(t);
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
        for (int i = 0; i < kCols; ++i) screen_.strip(P_COL0 + i, kColFrames);
        for (int i = 0; i < kPads; ++i) { screen_.strip(P_SLOTLIT0 + i, 3); screen_.strip(P_STEPLIT0 + i, 2); }
        for (int i = 0; i < kLanes * kPads; ++i) screen_.strip(P_CELL0 + i, 4);
        for (int i = 0; i < kLanes; ++i) screen_.strip(P_LAMP0 + i, 2);
        screen_.strip(P_REVLIT, 2);
        screen_.strip(P_LOCKLIT, 2);
        for (int p = P_NEWALL; p < P_ROWTAP0; ++p)
        {
            if (p == P_SCROLL) continue;
            int f = p - P_NEWALL;
            screen_.button(p, p < P_REVBTN && f < kNumFlash ? P_FLASH0 + f : -1);
        }
        for (int i = 0; i < kRows; ++i) screen_.button(P_ROWTAP0 + i);
        screen_.watch(P_SCROLL);
        screen_.watch(P_AUTO);

        for (auto& r : retired_) r.store(nullptr);
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
        i.version = 200;
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
        const bool autoMode = getInt(P_AUTO) == 0;
        const bool playing = tr.valid && tr.playing && tr.ppqValid;
        if (!playing || !seq_) swapSeq();

        genralink::LinkData* L = link_.data();
        const double bpm = tr.tempoValid && tr.bpm > 1.0 ? tr.bpm : 120.0;
        if (tr.tempoValid) hostBpmX100_.store((int)std::lround(tr.bpm * 100.0), std::memory_order_relaxed);
        if (L)
        {
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
        if (playing && seq_)
        {
            const double q0 = tr.ppqPos * 4.0;
            const double q1 = q0 + ctx.numFrames / spStep;
            if (!wasPlaying_ || nextStep_ < q0 - 1.0 || nextStep_ > q1 + 1.0) nextStep_ = std::ceil(q0 - 1e-6);
            while (nextStep_ < q1)
            {
                long n = (long)std::llround(nextStep_);
                int s = (int)(((n % kSteps) + kSteps) % kSteps);
                if (s % hh::STEPS == 0) swapSeq();
                curStep_.store(s, std::memory_order_relaxed);
                if (L) L->step.store(s, std::memory_order_relaxed);
                double at = (nextStep_ - q0) * spStep;
                if (seq_ && autoMode) scheduleStep(seq_->ev[s], s, (uint32_t)(n / kSteps), at, spStep, seq_->swing);
                nextStep_ += 1.0;
            }
            wasPlaying_ = true;
        }
        else
        {
            if (wasPlaying_)
                for (Voice& v : voices_) if (v.active && v.seq && v.release < 0) v.release = kFadeOut;
            nTrig_ = 0;
            wasPlaying_ = false;
            curStep_.store(-1, std::memory_order_relaxed);
            if (L) L->step.store(-1, std::memory_order_relaxed);
        }

        float target = std::pow(10.f, get(P_VOLUME) / 20.f);
        if (gainSmooth_ < 0.f) gainSmooth_ = target;
        float* outL = ctx.outputs[0];
        float* outR = ctx.numOutputs > 1 ? ctx.outputs[1] : nullptr;
        int ev = 0;
        const bool followView = getInt(P_VIEW) == 2;
        const double previewRate = preview_ ? (double)preview_->sampleRate / sr_ : 1.0;
        for (int i = 0; i < ctx.numFrames; ++i)
        {
            while (ev < ctx.numMidiEvents && ctx.midi[ev].sampleOffset <= i)
            {
                const MidiEvent& e = ctx.midi[ev++];
                if (e.isNoteOn()) noteOn(e.note(), e.velocity() / 127.f, bank, followView);
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
        pathRequest_ = true;
        set(P_BANK, 0.f);
        bankReset_.store(true);
        resyncReq_.store(true);
    }

    bool formatValue(int index, float plain, std::string& out) override
    {
        char b[32];
        if (index == P_FILTER)
        {
            if (plain >= 99.5f) { out = "offen"; return true; }
            float hz = cutoffHz(plain);
            if (hz >= 1000.f) std::snprintf(b, sizeof b, "LP %.1fk", hz / 1000.f);
            else std::snprintf(b, sizeof b, "LP %.0f", hz);
            out = b;
            return true;
        }
        if (index == P_PITCH) { std::snprintf(b, sizeof b, "%+d st", (int)std::lround(plain)); out = b; return true; }
        if (index == P_DECAY) { std::snprintf(b, sizeof b, plain >= 99.5f ? "voll" : "%.0f %%", plain); out = b; return true; }
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
    void noteOn(int note, float vel, int bank, bool select)
    {
        if (pads_.learnNote(note)) return;
        int pad = pads_.padOf(note);
        if (pad < 0) return;
        int chop = pad + kPads * bank;
        if (select) selReq_.store(chop, std::memory_order_relaxed);
        startVoice(chop, vel, 0, false, -1.0, false);
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
        }
    }

    void startVoice(int chop, float vel, int pitch, bool rev, double gate, bool fromSeq)
    {
        if (!cur_ || chop < 0 || chop >= cur_->info.count) return;
        lastPlayed_.store(chop, std::memory_order_relaxed);
        const bool choke = getBool(P_CHOKE);
        for (Voice& v : voices_)
            if (v.active && v.release < 0 && (choke || v.chop == chop)) v.release = kFadeOut / 2;
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
                env *= (float)v.release / kFadeOut;
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
            if (loadPath(path)) { cutAndPublish(true); screen_.message(P_LISTINFO, "geladen: " + stripWav(baseName(path))); }
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
            screen_.message(P_LISTINFO, "nicht lesbar: " + stripWav(baseName(path)));
            return;
        }
        for (auto& r : previewRetired_) delete r.exchange(nullptr);
        delete previewPending_.exchange(d, std::memory_order_acq_rel);
        previewPath_ = path;
        screen_.message(P_LISTINFO, "nochmal tippen = laden");
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
        screen_.setText(P_ANALYSE, "laedt ...");
        std::vector<uint8_t> bytes;
        if (!mpc::readFile(path, bytes)) { screen_.setText(P_ANALYSE, "Fehler: nicht lesbar"); screen_.message(P_STATUS, "Sample nicht lesbar"); return false; }
        hh::SampleData d;
        std::string err;
        if (!hh::readWav(bytes, d, err, 60.f)) { screen_.setText(P_ANALYSE, "Fehler: " + err); screen_.message(P_STATUS, "Fehler: " + err); return false; }
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
        if (restore) { publishLoop(); screen_.message(P_STATUS, "Projekt geladen"); }
        else
        {
            regenerate(-1, false);
            screen_.message(P_STATUS, newSample ? "Neues Sample: neuer Loop" : "Neu geschnitten: neuer Loop");
        }
    }

    // ------------------------------------------------------------------ worker: the loop
    float bpmNow() const
    {
        int h = hostBpmX100_.load();
        return h > 0 ? h / 100.f : hh::STYLES[clamp(getInt(P_STYLE), 0, hh::NUM_STYLES - 1)].bpm;
    }

    void regenerate(int track, bool keepUndo = true)
    {
        if (keepUndo)
        {
            if (track < 0) { undoPat_ = pat_; hasUndo_ = true; }
            else if (track < kLanes) { laneUndo_[track] = pat_; laneHasUndo_[track] = true; }
        }
        set_.style = clamp(getInt(P_STYLE), 0, hh::NUM_STYLES - 1);
        set_.key = info_.key;
        set_.keyAuto = true;
        seed_ = seed_ * 1664525u + 1013904223u;
        hh::generate(pat_, track, set_, info_, padSet_, seed_, track >= 0, bpmNow());
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

    void publishLoop()
    {
        hh::render(pat_, knobs_, rend_);
        {
            std::lock_guard<std::mutex> g(mtx_);
            savedPattern_ = pat_;
        }
        Seq* s = new Seq();
        for (int i = 0; i < kSteps; ++i) s->ev[i] = rend_.chop[i];
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
            l.bass[i] = note(pat_.bass[i]); // the base (DENSITY is GENRA BASS's own knob)
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
        screen_.setText(P_LOADED, sampleName_.empty() ? std::string("Testloop (eingebaut)") : sampleName_);
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
        else screen_.setText(P_STATUS, "kein Sample");

        int step = curStep_.load();
        int bar = step >= 0 ? step / hh::STEPS : 0;
        screen_.setText(P_BARTXT, step >= 0 ? "Takt " + std::to_string(bar + 1) + " / 4" : std::string("Stopp"));
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
            std::string name = "nicht da";
            if (L)
            {
                on = laneAlive_[k].check(L->lane[k].beat.load(), nowMs());
                if (on) { name = genralink::Link::readName(L->lane[k]); if (name.empty()) name = "verbunden"; }
                uint32_t rq = L->lane[k].newRequest.load(), uq = L->lane[k].undoRequest.load();
                if (!laneReqInit_[k]) { laneReq_[k] = rq; laneUndoReq_[k] = uq; laneReqInit_[k] = true; }
                if (rq != laneReq_[k]) { laneReq_[k] = rq; if (on) newLane(k); }
                if (uq != laneUndoReq_[k]) { laneUndoReq_[k] = uq; if (on) { undoLane(k); screen_.message(P_STATUS, "UNDO von Spur " + std::to_string(k + 1)); } }
            }
            if (on) ++alive;
            screen_.setFrame(P_LAMP0 + k, on ? 1 : 0);
            screen_.setText(P_LANETXT0 + k, name);
        }
        linked_ = alive;
        if (!L) screen_.setText(P_LINKINFO, "Fehler: " + link_.error());
        else screen_.setText(P_LINKINFO, alive ? std::to_string(alive) + " Plugin(s) verbunden" : std::string("kein Begleit-Plugin"));
    }

    void newLane(int k)
    {
        const char* names[kLanes] = {"Drums", "Bass", "Melodie", "Pads"};
        regenerate(k);
        screen_.message(P_STATUS, std::string("NEW ") + names[k] + ": ab dem naechsten Takt");
    }

    // ------------------------------------------------------------------ worker: files
    void saveChop()
    {
        int chop = sel_;
        if (chop < 0 || chop >= info_.count || src_.frames() == 0) { screen_.message(P_STATUS, "erst einen Chop waehlen"); return; }
        std::string dir;
        {
            std::lock_guard<std::mutex> g(mtx_);
            size_t i = loadedPath_.find_last_of('/');
            dir = i == std::string::npos ? genraDir() : loadedPath_.substr(0, i);
        }
        dir += "/GENRA CHOP";
        if (!saveDir_.empty() && isDir(saveDir_)) dir = saveDir_;
        ::mkdir(dir.c_str(), 0777);
        if (chopEnd_[chop] <= chopStart_[chop]) { screen_.message(P_STATUS, "Chop ist leer"); return; }
        std::vector<uint8_t> wav = hh::writeWav(src_, chopStart_[chop], chopEnd_[chop], cRev_[chop].load(), (int)std::lround(cPitch_[chop].load()));
        char name[96];
        std::snprintf(name, sizeof name, "/%s chop%02d.wav", sampleName_.substr(0, 60).c_str(), chop + 1);
        bool ok = writeFileBytes(dir + name, wav.data(), wav.size());
        screen_.message(P_STATUS, ok ? "Chop " + std::to_string(chop + 1) + " als WAV gespeichert" : std::string("Fehler beim Speichern"));
        screen_.setText(P_WRITE, ok ? "OK" : "Fehler: WAV");
    }

    // PRINT: the chop track as a MIDI file (pad notes as learned; chops above 16 play on the bank chosen)
    void print()
    {
        std::string dir = printDir(printDirChosen_);
        if (dir.empty()) { screen_.message(P_PRINTSTATUS, "kein Ordner zum Schreiben"); return; }
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
            screen_.message(P_PRINTSTATUS, std::string(name) + " gespeichert, jetzt PADS", 4000);
            screen_.message(P_STATUS, "PRINT fertig: jetzt PADS", 3000);
        }
        else screen_.message(P_PRINTSTATUS, "Fehler beim Schreiben");
        printTexts();
    }

    void printTexts()
    {
        std::string dir = printDir(printDirChosen_);
        screen_.setText(P_PRINTDIRTXT, dir.empty() ? std::string("kein Ordner") : pathLabel(dir), true);
        std::vector<std::string> list = dir.empty() ? std::vector<std::string>() : listPrints(dir);
        for (int r = 0; r < kPrintRows; ++r)
            screen_.setText(P_PRINTROW0 + r, r < (int)list.size() ? (r == 0 ? "> " : "") + list[(size_t)r].substr(0, list[(size_t)r].size() - 4) : std::string());
        char b[64];
        std::snprintf(b, sizeof b, "Chop-Spur · 4 Takte · %.0f BPM · %s", (double)bpmNow(), keyText().c_str());
        screen_.setText(P_PRINTINFO, b);
        screen_.setText(P_PRINTSTATUS, getInt(P_AUTO) == 0 ? "AUTO: der Loop spielt" : "PADS: nur die Noten der MPC-Spur");
    }

    void setupTexts()
    {
        screen_.setText(P_LEARNINFO, pads_.learning() ? pads_.info() : (pads_.learned() ? "gelernt" : "noch nicht gelernt"));
        screen_.setText(P_MAPINFO, pads_.info());
        screen_.setText(P_SAVEDIRTXT, saveDir_.empty() ? std::string("neben dem Sample (/GENRA CHOP)") : pathLabel(saveDir_), true);
        std::string names;
        for (const std::string& d : findDrives(false)) names += (names.empty() ? "" : ", ") + pathLabel(d);
        screen_.setText(P_DRIVES, names.empty() ? std::string("keins gefunden") : names);
        std::string g = genraDir();
        screen_.setText(P_WRITE, g.empty() ? std::string("kein Genra-Ordner") : "Genra-Ordner: " + pathLabel(g));
    }

    // ------------------------------------------------------------------ worker: buttons
    void handleTaps()
    {
        for (auto& t : screen_.taps())
        {
            const int p = t.first;
            if (p == P_NEWALL) { regenerate(-1); screen_.message(P_STATUS, "NEW ALL: neuer Loop ab dem naechsten Takt"); }
            else if (p >= P_NEW0 && p < P_NEW0 + kLanes) newLane(p - P_NEW0);
            else if (p == P_UNDO)
            {
                if (hasUndo_) { std::swap(pat_, undoPat_); publishLoop(); screen_.message(P_STATUS, "UNDO: voriger Loop"); }
                else screen_.message(P_STATUS, "nichts zum Rueckgaengigmachen");
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
            else if (p == P_REVBTN && sel_ < info_.count) { cRev_[sel_].store(!cRev_[sel_].load()); }
            else if (p == P_LOCKBTN && sel_ < info_.count) { padSet_[sel_].lock = !padSet_[sel_].lock; }
            else if (p == P_PRINT) print();
            else if (p == P_PRINTHERE)
            {
                if (browser_.cwd().empty() || browser_.cwd() == "*") screen_.message(P_PRINTSTATUS, "erst im BROWSER einen Ordner oeffnen");
                else { printDirChosen_ = browser_.cwd(); settings_.set("printdir", printDirChosen_); settings_.save(); screen_.message(P_PRINTSTATUS, "Print-Ordner gesetzt"); }
            }
            else if (p == P_PRINTSTD) { printDirChosen_.clear(); settings_.set("printdir", ""); settings_.save(); screen_.message(P_PRINTSTATUS, "Print-Ordner: Genra/PRINT"); }
            else if (p == P_LEARN) pads_.startLearning();
            else if (p == P_SCROLL) browser_.scroll(t.second > 0 ? 1 + (t.second - 1) / 6 : -1 - (-t.second - 1) / 6);
            else if (p == P_BACK) { browser_.up(); rememberDir(); stopPreview(); }
            else if (p == P_DRVBTN) { browser_.drives(); stopPreview(); }
            else if (p == P_PGUP) browser_.page(-1);
            else if (p == P_PGDN) browser_.page(1);
            else if (p == P_SAVEHERE)
            {
                if (browser_.cwd().empty() || browser_.cwd() == "*") screen_.message(P_LISTINFO, "erst einen Ordner oeffnen");
                else { saveDir_ = browser_.cwd(); settings_.set("savedir", saveDir_); settings_.save(); screen_.message(P_LISTINFO, "Speicherort gesetzt"); }
            }
            else if (p == P_AUTO) screen_.message(P_STATUS, getInt(P_AUTO) == 0 ? "AUTO: der Loop spielt" : "PADS: nur die Noten der MPC-Spur");
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
        clock::time_point cutChanged{}, lastSlow{};
        int lastMode = getInt(P_MODE), lastChops = getInt(P_CHOPS), lastStyle = getInt(P_STYLE);
        bool cutPending = false;
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
            if (wantLoad)
            {
                if (!want.empty() && loadPath(want)) cutAndPublish(true);
                else if (wasRestored) cutAndPublish(false);
            }
            handleTaps();
            if (bankReset_.exchange(false)) { notifyHostParameterChanged(P_BANK); requestDisplayUpdate(); }
            if (pads_.finishIfDone()) screen_.message(P_LEARNINFO, "gelernt und gespeichert", 3000);

            int req = selReq_.exchange(-1);
            if (req >= 0 && req < std::max(1, info_.count) && req != sel_) { sel_ = req; knobsFromChop(); }
            else knobsToChop();

            int m = getInt(P_MODE), c = getInt(P_CHOPS);
            if (m != lastMode || c != lastChops) { lastMode = m; lastChops = c; cutPending = true; cutChanged = now; }
            if (cutPending && now - cutChanged > std::chrono::milliseconds(400)) { cutPending = false; cutAndPublish(false); }
            int st = getInt(P_STYLE);
            if (st != lastStyle) { lastStyle = st; screen_.message(P_STATUS, "STYLE " + styleLabel(st) + ": NEW ALL druecken", 3000); }

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
    hh::SampleData* preview_ = nullptr;
    double previewPos_ = 0.0;
    int previewFade_ = -1;
    Voice voices_[kVoices];
    Trig trig_[48];
    int nTrig_ = 0;
    double sr_ = 44100.0;
    double nextStep_ = 0.0;
    bool wasPlaying_ = false;
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
};

MPC_PLUGIN_ENTRY(GenraChop)
