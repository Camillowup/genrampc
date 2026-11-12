// GENRA CHOP - the sample chopper of GENRA CHOP (VCV Rack) on the MPC, the chef of the GENRA plugins.
//
// SPDX-License-Identifier: GPL-3.0-only
//
// Pages (built by genra-chop/tools/build_skin.py), on the function keys:
//   BROWSER  path, ten rows to tap (folder: open, WAV: load), ZURUECK, LAUFWERKE, page buttons, LISTE (data wheel)
//   CHOP     header (sample, tempo and key, LINK, views SAMPLE / LOOP / CHOP), the display (a column waveform:
//            film strips, one parameter per column), the chop strip (16 chops of the bank: number, chord, length,
//            lit while playing, tap to play and select), the 16 steps of the current bar, the four lanes
//            (drums, bass, melody, pads: LINK lamp, the bar's notes, preset of the companion, NEW per lane),
//            STYLE, NEW ALL, MODUS, CHOPS, BANK, UNDO, SAVE WAV, CHOKE and the four knobs of the selected chop
//   SETUP    LERNEN (which notes the MPC's pads send) and the technical status
//
// The loop: NEW ALL rolls all tracks with the GENRA CHOP engine (src/Core.cpp, Chop.cpp, Styles.cpp, copied from
// Camillowup/genra-chop). This plugin plays the CHOP track (the sample's chops) on the MPC's transport (Play / Stop
// of the MPC, no transport of its own); a new loop starts at the next bar. Drums, bass, melody and pads go to the
// companion plugins on other tracks through LINK v2 (common/genra_link.h). GENRA BASS is the first of them.
//
// Threads: process() (audio) plays chops and the loop and only reads/writes atomics. A worker thread browses, loads,
// cuts, generates, writes files and makes the screen. The MPC re-reads a text only when it thinks the value changed,
// so the worker nudges every changed text parameter (audioMasterAutomate + audioMasterUpdateDisplay); film strips
// get their frame as the value.

#include "mpc_plugin.h"
#include "Chop.hpp"
#include "../../common/genra_link.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <dirent.h>
#include <mutex>
#include <sys/stat.h>
#include <thread>

using namespace mpc;

namespace {

static const int kRows = 10;   // rows of the browser
static const int kPads = 16;   // pads / chop slots per bank
static const int kCols = 124;  // columns of the display
static const int kLanes = 4;
static const int kColFrames = 48, kColH = 16;  // column strip: 3 states x 16 heights
static const int kSteps = hh::LEN;             // 64

// captions: fixed texts in a skin are not drawn on the MPC, texts from the plugin are; the parameter is "CAP <key>"
static const char* const kCaptions[][2] = {
    {"genra", "GENRA"}, {"chop", "CHOP"}, {"sample", "SAMPLE"}, {"loop", "LOOP"}, {"chopv", "CHOP"},
    {"style", "STYLE"}, {"newall", "NEW ALL"}, {"new", "NEW"}, {"modus", "MODUS"}, {"transient", "Transient"},
    {"equal", "Equal"}, {"chops", "CHOPS"}, {"minus", "-"}, {"plus", "+"}, {"bank", "BANK"},
    {"bankA", "A"}, {"bankB", "B"}, {"bankC", "C"}, {"bankD", "D"}, {"undo", "UNDO"}, {"savewav", "SAVE WAV"},
    {"drums", "DRUMS"}, {"bass", "BASS"}, {"mel", "MELODIE"}, {"pads", "PADS"}, {"prev", "<"}, {"next", ">"},
    {"back", "ZURÜCK"}, {"drives", "LAUFWERKE"}, {"pgup", "< SEITE"}, {"pgdn", "SEITE >"},
    {"savehere", "SPEICHERORT HIER"}, {"loaded", "Geladen:"}, {"learn", "LERNEN"},
    {"help1", "LERNEN antippen, dann die"}, {"help2", "Pads 1 bis 16 der Reihe nach"},
    {"help3", "druecken (Pad 1 = unten links)"},
    {"r1", "Pad-Belegung"}, {"r2", "Laufwerke"}, {"r3", "Schreiben"}, {"r4", "LINK"}, {"r5", "Analyse"},
    {"r6", "Speicherort"}};
static const int kNumCaps = (int)(sizeof(kCaptions) / sizeof(kCaptions[0]));

enum
{
    // the four knobs of the selected chop (first: Q-Links 1-4 in Program mode)
    P_PITCH = 0, P_DECAY, P_FILTER, P_LEVEL,
    P_VOLUME, P_STYLE, P_MODE, P_CHOPS, P_BANK, P_VIEW, P_CHOKE,
    // buttons: every tap flips them; the worker acts on a change that stays (the MPC probes values to read texts)
    P_NEWALL, P_NEW0,
    P_UNDO = P_NEW0 + kLanes, P_SAVE, P_CHMINUS, P_CHPLUS, P_STPREV, P_STNEXT, P_LEARN, P_SCROLL,
    P_BACK, P_DRVBTN, P_PGUP, P_PGDN, P_SAVEHERE,
    P_ROWTAP0,
    P_SLOTTAP0 = P_ROWTAP0 + kRows,    // the chop strip: played at once in process(), selected by the worker
    // texts (read only)
    P_PATH = P_SLOTTAP0 + kPads,
    P_LISTINFO, P_LOADED, P_HDRKEY, P_LINKTXT, P_CHOPSTXT, P_CHOKETXT, P_SEL1, P_SEL2, P_BARTXT, P_STATUS,
    P_LEARNINFO, P_MAPINFO, P_DRIVES, P_WRITE, P_LINKINFO, P_ANALYSE, P_SAVEDIRTXT,
    P_SLOTTXT0,
    P_SLOTLEN0 = P_SLOTTXT0 + kPads,
    P_LANETXT0 = P_SLOTLEN0 + kPads,
    P_ROWT0 = P_LANETXT0 + kLanes,
    P_CAP0 = P_ROWT0 + kRows,
    // film strips: the value is the frame
    P_COL0 = P_CAP0 + kNumCaps,
    P_SLOTLIT0 = P_COL0 + kCols,       // 0 off, 1 selected, 2 playing
    P_STEPLIT0 = P_SLOTLIT0 + kPads,   // 0 off, 1 the step that plays
    P_CELL0 = P_STEPLIT0 + kPads,      // lane x step: 0 empty, 1 note, +2 the step that plays
    P_LAMP0 = P_CELL0 + kLanes * kPads,// 0 no companion, 1 linked
    P_COUNT = P_LAMP0 + kLanes
};
static const int kFirstButton = P_NEWALL, kNumButtons = P_ROWTAP0 + kRows - P_NEWALL; // incl. LISTE and the rows
static const int kFirstText = P_PATH, kFirstStrip = P_COL0;

int stripFrames(int p)
{
    if (p >= P_COL0 && p < P_SLOTLIT0) return kColFrames;
    if (p >= P_SLOTLIT0 && p < P_STEPLIT0) return 3;
    if (p >= P_STEPLIT0 && p < P_CELL0) return 2;
    if (p >= P_CELL0 && p < P_LAMP0) return 4;
    return 2;
}

static const int kVoices = 12;
static const int kFadeIn = 32;
static const int kFadeOut = 96;

struct Voice
{
    bool active = false, seq = false, rev = false;
    int chop = -1;
    double pos = 0.0, a = 0.0, b = 0.0, rate = 1.0; // b: end (forward) / a: start (reverse)
    double gate = -1.0;   // samples until release (loop notes), -1: the chop's length
    double decayLen = 0.0;// samples of the decay (0: none)
    float gain = 1.f;
    float fa = 1.f;       // filter coefficient (1: open)
    float zl1 = 0.f, zl2 = 0.f, zr1 = 0.f, zr2 = 0.f;
    int age = 0;
    int release = -1;
};

struct Trig
{
    double at;   // frames from the start of this block
    int chop;
    float vel;
    int pitch;
    bool rev;
    double gate;
};

// what the audio thread plays as the loop
struct Seq
{
    hh::ChopEv ev[hh::LEN];
    float swing = 0.5f;
};

std::string baseName(const std::string& p)
{
    size_t i = p.find_last_of('/');
    return i == std::string::npos ? p : p.substr(i + 1);
}

std::string stripWav(const std::string& n)
{
    if (n.size() > 4)
    {
        std::string e = n.substr(n.size() - 4);
        std::transform(e.begin(), e.end(), e.begin(), ::tolower);
        if (e == ".wav") return n.substr(0, n.size() - 4);
    }
    return n;
}

bool isDir(const std::string& p)
{
    struct stat st;
    return ::stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool lessNoCase(const std::string& a, const std::string& b)
{
    size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i)
    {
        int x = ::tolower((unsigned char)a[i]), y = ::tolower((unsigned char)b[i]);
        if (x != y) return x < y;
    }
    return a.size() < b.size();
}

// Entries of a folder: sub folders or .wav files (no hidden entries, no Windows system folders).
std::vector<std::string> listDir(const std::string& dir, bool wantDirs)
{
    std::vector<std::string> out;
    DIR* d = ::opendir(dir.c_str());
    if (!d) return out;
    while (dirent* e = ::readdir(d))
    {
        std::string n = e->d_name;
        if (n.empty() || n[0] == '.' || n[0] == '$') continue;
        std::string full = dir + "/" + n;
        bool dirp = isDir(full);
        if (wantDirs && dirp)
        {
            if (n == "lost+found" || n == "System Volume Information" || n == "VST2 Plugins") continue;
            out.push_back(n);
        }
        else if (!wantDirs && !dirp && stripWav(n) != n)
            out.push_back(n);
        if (out.size() >= 2000) break;
    }
    ::closedir(d);
    std::sort(out.begin(), out.end(), lessNoCase);
    return out;
}

bool writeFileBytes(const std::string& path, const void* data, size_t n)
{
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = std::fwrite(data, 1, n, f) == n;
    ok = (std::fclose(f) == 0) && ok;
    return ok;
}

// "/nvme/Samples" -> "SSD/Samples", "/media/Archive/x" -> "Archive/x"
std::string pathLabel(const std::string& p)
{
    if (p == "*") return "(alle Mounts)";
    if (p.compare(0, 5, "/nvme") == 0) return "SSD" + p.substr(5);
    for (const char* pre : {"/media/", "/mnt/", "/run/media/"})
    {
        size_t n = std::strlen(pre);
        if (p.compare(0, n, pre) == 0) return p.substr(n);
    }
    return p;
}

// Every mount point that can hold samples (from /proc/mounts); all = every mount except /, /proc, /sys, /dev.
std::vector<std::string> findDrives(bool all)
{
    std::vector<std::string> out;
    if (const char* env = std::getenv("CHOP_TEST_DRIVE")) if (isDir(env)) out.push_back(env); // tests on a computer
    std::string t;
    if (mpc::readTextFile("/proc/mounts", t))
    {
        size_t pos = 0;
        while (pos < t.size())
        {
            size_t e = t.find('\n', pos);
            std::string line = t.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
            pos = e == std::string::npos ? t.size() : e + 1;
            size_t a = line.find(' ');
            if (a == std::string::npos) continue;
            size_t b = line.find(' ', a + 1);
            std::string mnt = line.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1);
            std::string m; // /proc/mounts writes a space as \040
            for (size_t i = 0; i < mnt.size(); ++i)
            {
                if (mnt[i] == '\\' && i + 3 < mnt.size()) { m += (char)std::strtol(mnt.substr(i + 1, 3).c_str(), nullptr, 8); i += 3; }
                else m += mnt[i];
            }
            bool keep = all ? (m != "/" && m.compare(0, 5, "/proc") != 0 && m.compare(0, 4, "/sys") != 0 && m.compare(0, 4, "/dev") != 0)
                            : (m == "/nvme" || (m.size() > 7 && m.compare(0, 7, "/media/") == 0) || (m.size() > 5 && m.compare(0, 5, "/mnt/") == 0)
                               || (m.size() > 11 && m.compare(0, 11, "/run/media/") == 0));
            if (keep && isDir(m) && std::find(out.begin(), out.end(), m) == out.end()) out.push_back(m);
        }
    }
    if (!all && isDir("/nvme") && std::find(out.begin(), out.end(), std::string("/nvme")) == out.end()) out.insert(out.begin(), "/nvme");
    return out;
}

// A built-in test loop (2 bars at 92 BPM, F#m7 / Bm7 stabs with a kick and hats), so the pads sound even
// when no WAV could be found.
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

std::string chordText(const hh::SampleInfo& info, int chop)
{
    if (chop < 0 || chop >= info.count) return std::string();
    hh::Chord ch = hh::padChord(info, nullptr, chop);
    if (!ch.valid()) return "HIT";
    char c[16];
    hh::chordName(ch, c, sizeof c);
    return c;
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
    GenraChop() : Plugin(makeInfo())
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
        addButton("newall", "NEW ALL");
        const char* lanes[kLanes] = {"DRUMS", "BASS", "MELODIE", "PADS"};
        for (int k = 0; k < kLanes; ++k) addButton(("new" + std::to_string(k + 1)).c_str(), (std::string("NEW ") + lanes[k]).c_str());
        addButton("undo", "UNDO");
        addButton("save", "SAVE WAV");
        addButton("chminus", "CHOPS MINUS");
        addButton("chplus", "CHOPS PLUS");
        addButton("stprev", "STYLE ZURUECK");
        addButton("stnext", "STYLE VOR");
        addButton("learn", "LERNEN");
        addParam(intParam("scroll", "LISTE", 0, 1000, 500));
        addButton("back", "ZURUECK");
        addButton("drvbtn", "LAUFWERKE");
        addButton("pgup", "SEITE ZURUECK");
        addButton("pgdn", "SEITE VOR");
        addButton("savehere", "SPEICHERORT HIER");
        for (int i = 0; i < kRows; ++i) addButton(("rowtap" + std::to_string(i + 1)).c_str(), ("TIPP " + std::to_string(i + 1)).c_str());
        for (int i = 0; i < kPads; ++i) addButton(("slottap" + std::to_string(i + 1)).c_str(), ("SLOT " + std::to_string(i + 1)).c_str());
        const char* texts[] = {"PFAD", "LISTINFO", "GELADEN", "KOPF", "LINKTEXT", "CHOPZAHL", "CHOKETEXT", "AUSWAHL 1",
                               "AUSWAHL 2", "TAKT", "STATUS", "LERNINFO", "BELEGUNG", "LAUFWERKSLISTE", "SCHREIBEN",
                               "LINKINFO", "ANALYSE", "SPEICHERORT"};
        for (const char* t : texts) addText(t);
        for (int i = 0; i < kPads; ++i) addText(("SLOTTEXT " + std::to_string(i + 1)).c_str());
        for (int i = 0; i < kPads; ++i) addText(("SLOTLAENGE " + std::to_string(i + 1)).c_str());
        for (int i = 0; i < kLanes; ++i) addText(("SPURTEXT " + std::to_string(i + 1)).c_str());
        for (int i = 0; i < kRows; ++i) addText(("ZEILE " + std::to_string(i + 1)).c_str());
        for (const auto& c : kCaptions) addText((std::string("CAP ") + c[0]).c_str());
        for (int i = 0; i < kCols; ++i) addText(("SPALTE " + std::to_string(i + 1)).c_str());
        for (int i = 0; i < kPads; ++i) addText(("SLOTLICHT " + std::to_string(i + 1)).c_str());
        for (int i = 0; i < kPads; ++i) addText(("SCHRITT " + std::to_string(i + 1)).c_str());
        for (int k = 0; k < kLanes; ++k)
            for (int i = 0; i < kPads; ++i) addText(("ZELLE " + std::to_string(k + 1) + " " + std::to_string(i + 1)).c_str());
        for (int k = 0; k < kLanes; ++k) addText(("LAMPE " + std::to_string(k + 1)).c_str());
        if (numParams() != P_COUNT) std::abort(); // the enum and the list above must agree (host_sim catches it)

        texts_.assign(P_COUNT, std::string());
        for (int i = 0; i < kNumCaps; ++i) texts_[(size_t)(P_CAP0 + i)] = kCaptions[i][1];
        shown_.assign(P_COUNT, std::string("\x01"));
        frame_.assign(P_COUNT, 0);
        frameShown_.assign(P_COUNT, -1);
        for (auto& r : retired_) r.store(nullptr);
        for (auto& r : seqRetired_) r.store(nullptr);
        for (auto& n : noteToPad_) n.store(-1);
        for (int c = 0; c < hh::PADS; ++c) { cPitch_[c].store(0.f); cDecay_[c].store(100.f); cFilter_[c].store(100.f); cLevel_[c].store(0.f); }
        resyncButtons();
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
    }

    static PluginInfo makeInfo()
    {
        PluginInfo i;
        i.name = "GENRA CHOP";
        i.vendor = "Genra";
        i.uniqueId = MPC_FOURCC('G', 'n', 'C', 'h');
        i.version = 100;
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
            loadPadMap();
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
        const Transport& tr = ctx.transport;
        const bool playing = tr.valid && tr.playing && tr.ppqValid;
        if (!playing || !seq_) swapSeq(); // stopped (or none yet): a new loop takes over at once

        genralink::LinkData* L = link_.data();
        const double bpm = tr.tempoValid && tr.bpm > 1.0 ? tr.bpm : 120.0;
        if (tr.tempoValid) hostBpmX100_.store((int)std::lround(tr.bpm * 100.0), std::memory_order_relaxed);
        if (L)
        {
            L->writerBeat.fetch_add(1, std::memory_order_relaxed);
            if (tr.tempoValid) L->bpmX100.store((int32_t)std::lround(tr.bpm * 100.0), std::memory_order_relaxed);
        }
        const int bank = clamp(getInt(P_BANK), 0, 3);

        // the chop strip tapped on the screen (button parameters flip on every tap)
        for (int k = 0; k < kPads; ++k)
        {
            int v = getInt(P_SLOTTAP0 + k);
            if (v != slotTapSeen_[k])
            {
                slotTapSeen_[k] = v;
                lastNote_.store(-1, std::memory_order_relaxed);
                int chop = k + kPads * bank;
                selReq_.store(chop, std::memory_order_relaxed);
                startVoice(chop, 0.8f, 0, false, -1.0, false);
            }
        }

        // the loop on the MPC's transport: every 16th that falls into this block
        const double spStep = sr_ * 60.0 / bpm / 4.0; // samples per 16th
        if (playing && seq_)
        {
            const double q0 = tr.ppqPos * 4.0;          // position in 16ths
            const double q1 = q0 + ctx.numFrames / spStep;
            if (!wasPlaying_ || nextStep_ < q0 - 1.0 || nextStep_ > q1 + 1.0) nextStep_ = std::ceil(q0 - 1e-6);
            while (nextStep_ < q1)
            {
                long n = (long)std::llround(nextStep_);
                int s = (int)(((n % kSteps) + kSteps) % kSteps);
                if (s % hh::STEPS == 0) swapSeq(); // NEW ALL: the new loop starts with the bar
                curStep_.store(s, std::memory_order_relaxed);
                if (L) L->step.store(s, std::memory_order_relaxed);
                double at = (nextStep_ - q0) * spStep;
                if (seq_) scheduleStep(seq_->ev[s], s, (uint32_t)(n / kSteps), at, spStep, seq_->swing);
                nextStep_ += 1.0;
            }
            wasPlaying_ = true;
        }
        else if (wasPlaying_ || !playing)
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
        std::string head = "GC1\n" + loadedPath_ + "\n";
        out.assign(head.begin(), head.end());
        auto put = [&out](const void* p, size_t n) { const uint8_t* b = (const uint8_t*)p; out.insert(out.end(), b, b + n); };
        uint32_t size = (uint32_t)sizeof(hh::Pattern);
        put(&size, 4);
        put(&savedPattern_, sizeof(hh::Pattern));
        for (int c = 0; c < hh::PADS; ++c)
        {
            float v[4] = {cPitch_[c].load(), cDecay_[c].load(), cFilter_[c].load(), cLevel_[c].load()};
            put(v, sizeof v);
        }
    }

    void loadExtraState(const uint8_t* data, size_t size) override
    {
        std::lock_guard<std::mutex> g(mtx_);
        std::string s(reinterpret_cast<const char*>(data), size);
        if (s.compare(0, 4, "GC1\n") != 0) return;
        size_t e = s.find('\n', 4);
        if (e == std::string::npos) return;
        requestedPath_ = s.substr(4, e - 4);
        size_t pos = e + 1;
        uint32_t psize = 0;
        if (pos + 4 <= size) std::memcpy(&psize, data + pos, 4);
        pos += 4;
        if (psize == sizeof(hh::Pattern) && pos + psize + hh::PADS * 16 <= size)
        {
            std::memcpy(&restoredPattern_, data + pos, psize);
            restored_ = true;
            pos += psize;
            for (int c = 0; c < hh::PADS; ++c)
            {
                float v[4];
                std::memcpy(v, data + pos + (size_t)c * 16, 16);
                cPitch_[c].store(v[0]); cDecay_[c].store(v[1]); cFilter_[c].store(v[2]); cLevel_[c].store(v[3]);
            }
        }
        pathRequest_ = true;
        set(P_BANK, 0.f);
        bankReset_.store(true);
        resyncButtons();
    }

    // ------------------------------------------------------------------ screen texts (UI thread)
    bool formatValue(int index, float plain, std::string& out) override
    {
        if (index == P_FILTER)
        {
            if (plain >= 99.5f) { out = "offen"; return true; }
            char b[24];
            float hz = cutoffHz(plain);
            if (hz >= 1000.f) std::snprintf(b, sizeof b, "LP %.1fk", hz / 1000.f);
            else std::snprintf(b, sizeof b, "LP %.0f", hz);
            out = b;
            return true;
        }
        if (index == P_PITCH || index == P_DECAY || index == P_LEVEL)
        {
            char b[24];
            if (index == P_PITCH) std::snprintf(b, sizeof b, "%+d st", (int)std::lround(plain));
            else if (index == P_DECAY) std::snprintf(b, sizeof b, plain >= 99.5f ? "voll" : "%.0f %%", plain);
            else std::snprintf(b, sizeof b, "%+.1f dB", plain);
            out = b;
            return true;
        }
        if (index == P_SCROLL) { std::lock_guard<std::mutex> g(mtx_); out = texts_[P_LISTINFO]; return true; }
        if (index < kFirstText || index >= kFirstStrip) return false;
        std::lock_guard<std::mutex> g(mtx_);
        out = texts_[(size_t)index];
        return true;
    }

private:
    static float cutoffHz(float v) { return 120.f * std::pow(2.f, v / 100.f * 7.2f); } // 120 Hz .. 17 kHz

    // ------------------------------------------------------------------ parameters
    void addText(const char* name)
    {
        std::string id = name;
        for (char& c : id) c = c == ' ' ? '_' : (char)::tolower((unsigned char)c);
        Param p = floatParam(id.c_str(), name, 0.f, 1.f, 0.f);
        p.automatable = false;
        addParam(p);
    }
    void addButton(const char* id, const char* name) { addParam(boolParam(id, name, false)); }

    void resyncButtons()
    {
        for (int i = 0; i < kNumButtons; ++i) btnSeen_[i] = btnPrev_[i] = getInt(kFirstButton + i);
    }

    // ------------------------------------------------------------------ audio helpers
    void noteOn(int note, float vel, int bank, bool select)
    {
        lastNote_.store(note, std::memory_order_relaxed);
        if (learning_.load(std::memory_order_acquire))
        {
            int step = learnStep_.load(std::memory_order_relaxed);
            if (step < kPads)
            {
                bool used = false;
                for (int k = 0; k < step; ++k) if (learnNotes_[k].load(std::memory_order_relaxed) == note) used = true;
                if (!used)
                {
                    learnNotes_[step].store(note, std::memory_order_relaxed);
                    learnStep_.store(step + 1, std::memory_order_release);
                }
            }
            return;
        }
        int chop = -1;
        int pad = noteToPad_[note & 127].load(std::memory_order_relaxed);
        if (pad >= 0) chop = pad + kPads * bank;
        else if (!mapLearned_.load(std::memory_order_relaxed) && note >= 36 && note < 100) chop = note - 36;
        if (chop < 0) { lastChop_.store(-2, std::memory_order_relaxed); return; }
        if (select) selReq_.store(chop, std::memory_order_relaxed); // CHOP view: the display follows the pads
        startVoice(chop, vel, 0, false, -1.0, false);
    }

    // one step of the loop: swing, timing, chance, every-2nd, ratchets
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
        if (!cur_ || chop < 0 || chop >= cur_->info.count) { lastChop_.store(-1 - 1000 - chop, std::memory_order_relaxed); return; }
        lastChop_.store(chop, std::memory_order_relaxed);
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
        v.rev = rev;
        v.chop = chop;
        v.a = (double)cur_->start[chop];
        v.b = (double)cur_->end[chop];
        v.pos = rev ? v.b - 2.0 : v.a;
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
        // no free place: leak rather than free in the audio thread (does not happen at the worker's pace)
    }

    // ------------------------------------------------------------------ worker: texts and strips
    static std::string fit(const std::string& s, bool keepEnd = false)
    {
        if (s.size() <= 31) return s;
        return keepEnd ? "..." + s.substr(s.size() - 28) : s.substr(0, 30) + "~";
    }
    void setText(int index, const std::string& s, bool keepEnd = false)
    {
        std::lock_guard<std::mutex> g(mtx_);
        texts_[(size_t)index] = fit(s, keepEnd);
    }
    void setFrame(int index, int f) { frame_[(size_t)index] = f; }

    void pushScreen()
    {
        bool any = false;
        for (int i = kFirstText; i < kFirstStrip; ++i)
        {
            std::string now;
            {
                std::lock_guard<std::mutex> g(mtx_);
                now = texts_[(size_t)i];
            }
            if (now == shown_[(size_t)i]) continue;
            shown_[(size_t)i] = now;
            setNormalised(i, getNormalised(i) > 0.5f ? 0.f : 1.f);
            notifyHostParameterChanged(i);
            any = true;
        }
        for (int i = kFirstStrip; i < P_COUNT; ++i)
        {
            int f = frame_[(size_t)i];
            if (f == frameShown_[(size_t)i]) continue;
            frameShown_[(size_t)i] = f;
            setNormalised(i, (float)f / (float)(stripFrames(i) - 1));
            notifyHostParameterChanged(i);
            any = true;
        }
        if (any) requestDisplayUpdate();
    }

    void setParamFromWorker(int p, float plain)
    {
        set(p, plain);
        notifyHostParameterChanged(p);
    }

    // ------------------------------------------------------------------ worker: the browser
    struct Entry { std::string label, path; int kind; }; // kind: 0 up, 1 folder, 2 wav, 3 drive

    void listDirNow(const std::string& select = std::string())
    {
        entries_.clear();
        if (cwd_.empty() || cwd_ == "*")
        {
            if (cwd_ == "*") entries_.push_back({"..", "", 0});
            for (const std::string& d : findDrives(cwd_ == "*")) entries_.push_back({pathLabel(d), d, 3});
            if (cwd_.empty()) entries_.push_back({"(alle Mounts)", "*", 1});
        }
        else
        {
            entries_.push_back({"..", "", 0});
            for (const std::string& n : listDir(cwd_, true)) entries_.push_back({n, cwd_ + "/" + n, 1});
            for (const std::string& n : listDir(cwd_, false)) entries_.push_back({n, cwd_ + "/" + n, 2});
        }
        top_ = 0;
        if (!select.empty())
            for (size_t i = 0; i < entries_.size(); ++i)
                if (entries_[i].path == select) { top_ = std::max(0, (int)i - kRows / 2); break; }
        clampTop();
    }

    void clampTop() { top_ = clamp(top_, 0, std::max(0, (int)entries_.size() - kRows)); }

    std::string parentOf(const std::string& p) const
    {
        for (const std::string& d : findDrives(false)) if (p == d) return std::string();
        size_t i = p.find_last_of('/');
        return i == std::string::npos || i == 0 ? std::string() : p.substr(0, i);
    }

    void goUp()
    {
        if (cwd_.empty()) return;
        std::string from = cwd_;
        cwd_ = cwd_ == "*" ? std::string() : parentOf(cwd_);
        listDirNow(from);
        rememberDir();
    }

    void rememberDir() { if (cwd_ != "*") { lastDir_ = cwd_; lastDirDirty_ = true; } }

    void tapRow(int row)
    {
        int i = top_ + row;
        if (i < 0 || i >= (int)entries_.size()) return;
        const Entry e = entries_[(size_t)i];
        if (e.kind == 0) goUp();
        else if (e.kind == 1 || e.kind == 3) { cwd_ = e.path; listDirNow(); rememberDir(); }
        else if (loadPath(e.path)) cutAndPublish(true);
    }

    void browserTexts()
    {
        std::string loaded;
        {
            std::lock_guard<std::mutex> g(mtx_);
            loaded = loadedPath_;
        }
        for (int r = 0; r < kRows; ++r)
        {
            size_t i = (size_t)(top_ + r);
            std::string t;
            if (i < entries_.size())
            {
                const Entry& e = entries_[i];
                if (e.kind == 0) t = "..  (zurueck)";
                else if (e.kind == 1 || e.kind == 3) t = "[" + e.label + "]";
                else t = ((!loaded.empty() && e.path == loaded) ? "> " : "") + e.label;
            }
            setText(P_ROWT0 + r, t);
        }
        setText(P_PATH, cwd_.empty() ? std::string("Laufwerke") : pathLabel(cwd_), true);
        char b[64];
        int n = (int)entries_.size();
        if (n == 0) std::snprintf(b, sizeof b, "leer");
        else std::snprintf(b, sizeof b, "Zeile %d-%d von %d", top_ + 1, std::min(n, top_ + kRows), n);
        setText(P_LISTINFO, b);
    }

    // ------------------------------------------------------------------ worker: sample and cut
    bool loadPath(const std::string& path)
    {
        setText(P_ANALYSE, "laedt ...");
        std::vector<uint8_t> bytes;
        if (!mpc::readFile(path, bytes)) { setText(P_ANALYSE, "Fehler: nicht lesbar"); setText(P_STATUS, "Sample nicht lesbar"); return false; }
        hh::SampleData d;
        std::string err;
        if (!hh::readWav(bytes, d, err, 60.f)) { setText(P_ANALYSE, "Fehler: " + err); setText(P_STATUS, "Fehler: " + err); return false; }
        d.path = path;
        d.name = stripWav(baseName(path));
        src_ = std::move(d);
        std::lock_guard<std::mutex> g(mtx_);
        loadedPath_ = path;
        return true;
    }

    // cut and analyse; newSample: per-chop knobs back to neutral. Then a new loop (or the project's).
    void cutAndPublish(bool newSample)
    {
        if (src_.frames() == 0) return;
        auto t0 = std::chrono::steady_clock::now();
        hh::ChopSet* cs = new hh::ChopSet();
        cs->data = src_;
        hh::analyse(*cs, getInt(P_MODE) == 1 ? hh::CM_EQUAL : hh::CM_TRANSIENT, clamp(getInt(P_CHOPS), 4, 64));
        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

        info_ = cs->info;
        sampleBpm_ = cs->bpm;
        for (int p = 0; p < hh::PADS; ++p)
        {
            chopStart_[p] = p < cs->info.count ? cs->start[p] : 0;
            chopEnd_[p] = p < cs->info.count ? cs->end[p] : 0;
        }
        computeColumnsSource();
        for (auto& r : retired_) delete r.exchange(nullptr);
        delete pending_.exchange(cs, std::memory_order_acq_rel);

        char buf[64];
        std::snprintf(buf, sizeof buf, "OK, %d Chops in %.2f s", info_.count, secs);
        setText(P_ANALYSE, buf);
        sampleName_ = src_.name;

        bool restore = false;
        {
            std::lock_guard<std::mutex> g(mtx_);
            if (restored_) { restore = true; restored_ = false; pat_ = restoredPattern_; }
        }
        if (newSample && !restore)
            for (int c = 0; c < hh::PADS; ++c) { cPitch_[c].store(0.f); cDecay_[c].store(100.f); cFilter_[c].store(100.f); cLevel_[c].store(0.f); }
        sel_ = std::min(std::max(0, sel_), std::max(0, info_.count - 1));
        knobsFromChop();

        if (genralink::LinkData* L = link_.data())
        {
            L->keyRoot.store(info_.key.root);
            L->keyMinor.store(info_.key.minor ? 1 : 0);
            L->chopCount.store(info_.count);
            for (int p = 0; p < genralink::kChops; ++p)
            {
                hh::Chord c = hh::padChord(info_, nullptr, p);
                L->chordRoot[p].store((int8_t)(p < info_.count && c.valid() ? c.root : -1));
                L->chordQuality[p].store((int8_t)(p < info_.count && c.valid() ? c.quality : -1));
            }
        }
        if (restore) { publishLoop(); setText(P_STATUS, "Projekt geladen"); }
        else
        {
            regenerate(-1, false);
            setText(P_STATUS, newSample ? "Neues Sample: neuer Loop" : "Neu geschnitten: neuer Loop");
        }
    }

    // ------------------------------------------------------------------ worker: the loop
    float bpmNow() const
    {
        int h = hostBpmX100_.load();
        return h > 0 ? h / 100.f : hh::STYLES[clamp(getInt(P_STYLE), 0, hh::NUM_STYLES - 1)].bpm;
    }

    // NEW ALL (track -1) or NEW of one lane / track
    void regenerate(int track, bool keepUndo = true)
    {
        if (keepUndo) { undoPat_ = pat_; hasUndo_ = true; }
        set_.style = clamp(getInt(P_STYLE), 0, hh::NUM_STYLES - 1);
        set_.key = info_.key;
        set_.keyAuto = true;
        seed_ = seed_ * 1664525u + 1013904223u;
        hh::generate(pat_, track, set_, info_, nullptr, seed_, track >= 0, bpmNow());
        publishLoop();
    }

    void publishLoop()
    {
        hh::render(pat_, knobs_, rend_);
        savedPatternLocked();
        Seq* s = new Seq();
        for (int i = 0; i < kSteps; ++i) s->ev[i] = rend_.chop[i];
        s->swing = hh::STYLES[clamp(set_.style, 0, hh::NUM_STYLES - 1)].swing;
        for (auto& r : seqRetired_) delete r.exchange(nullptr);
        delete seqPending_.exchange(s, std::memory_order_acq_rel);

        genralink::Loop l;
        std::memset(&l, 0xff, sizeof l);
        auto note = [](const hh::Note& n) {
            genralink::StepNote o;
            o.pitch = n.pitch;
            o.len16 = (uint8_t)clamp((int)std::lround(n.len * 16.f), 1, 255);
            o.vel = (uint8_t)clamp((int)std::lround(n.vel * 127.f), 1, 127);
            o.flags = (uint8_t)((n.acc ? 1 : 0) | (n.slide ? 2 : 0));
            return o;
        };
        for (int i = 0; i < kSteps; ++i)
        {
            l.bass[i] = note(rend_.bass[i]);
            l.mel[i] = note(rend_.mel[i]);
            for (int v = 0; v < 4; ++v) l.pad[i][v] = rend_.pad[i].pitch[v];
            l.padLen[i] = (uint8_t)clamp((int)std::lround(rend_.pad[i].len), 1, 64);
            uint8_t d = 0;
            for (int v = 0; v < hh::DRUMS; ++v) if (rend_.drum[v][i].vel > 0.f) d |= (uint8_t)(1u << v);
            l.drums[i] = d;
        }
        for (int b = 0; b < genralink::kBars; ++b) { l.barRoot[b] = (int8_t)pat_.bar[b].root; l.barQuality[b] = (int8_t)pat_.bar[b].quality; }
        l.swingX1000 = (uint16_t)std::lround(s->swing * 1000.f);
        l.style = (int16_t)set_.style;
        link_.writeLoop(l);
        loopDirty_ = true;
    }

    void savedPatternLocked()
    {
        std::lock_guard<std::mutex> g(mtx_);
        savedPattern_ = pat_;
    }

    // ------------------------------------------------------------------ worker: the selected chop and its knobs
    void knobsFromChop()
    {
        int c = clamp(sel_, 0, hh::PADS - 1);
        shownKnob_[0] = cPitch_[c].load(); shownKnob_[1] = cDecay_[c].load();
        shownKnob_[2] = cFilter_[c].load(); shownKnob_[3] = cLevel_[c].load();
        for (int k = 0; k < 4; ++k) setParamFromWorker(P_PITCH + k, shownKnob_[k]);
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
    // peaks of the sample in fine parts (for the columns of every view)
    void computeColumnsSource()
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

    // states: 0 plain, 1 marker (a chop starts here), 2 selected chop
    void displayColumns()
    {
        const int view = getInt(P_VIEW);
        const size_t n = src_.frames();
        const int count = info_.count;
        if (!n || !count) { for (int c = 0; c < kCols; ++c) setFrame(P_COL0 + c, 0); return; }
        if (view == 2) // CHOP: the selected chop alone
        {
            int s = clamp(sel_, 0, count - 1);
            size_t a = chopStart_[s], b = std::max(a + 1, chopEnd_[s]);
            for (int c = 0; c < kCols; ++c)
            {
                size_t f = a + (b - a) * (size_t)c / kCols;
                setFrame(P_COL0 + c, (c == 0 ? 1 : 2) * kColH + heightOf(peakAt(f)));
            }
            return;
        }
        if (view == 1) // LOOP: the 64 steps, each column shows the chop sounding there
        {
            const float bpm = bpmNow();
            const double stepFrames = src_.sampleRate * 60.0 / bpm / 4.0;
            int curChop = -1, curStart = 0;
            float curLen = 1.f;
            int lastStepSeen = -1;
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
                setFrame(P_COL0 + c, state * kColH + h);
            }
            return;
        }
        // SAMPLE: the whole sample with the chop markers
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
            setFrame(P_COL0 + c, state * kColH + heightOf(m));
        }
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
            setText(P_SLOTTXT0 + k, has ? std::to_string(chop + 1) + " · " + chordText(info_, chop) : std::string());
            setText(P_SLOTLEN0 + k, has ? lengthText(info_.chop[chop].seconds, bpm) : std::string());
            setFrame(P_SLOTLIT0 + k, ((playing >> chop) & 1u) ? 2 : (chop == sel_ ? 1 : 0));
        }
        char key[16];
        hh::keyName(info_.key, key, sizeof key);
        char buf[64];
        std::snprintf(buf, sizeof buf, "%.0f BPM · %s", (double)bpm, key);
        setText(P_HDRKEY, buf);
        setText(P_LOADED, sampleName_.empty() ? std::string("Testloop (eingebaut)") : sampleName_);
        setText(P_CHOPSTXT, std::to_string(clamp(getInt(P_CHOPS), 4, 64)));
        setText(P_CHOKETXT, getBool(P_CHOKE) ? "CHOKE an" : "CHOKE aus");
        int s = clamp(sel_, 0, std::max(0, info_.count - 1));
        if (info_.count > 0)
        {
            setText(P_SEL1, "CHOP " + std::to_string(s + 1) + " · " + chordText(info_, s));
            std::snprintf(buf, sizeof buf, "%s · %.2f s", lengthText(info_.chop[s].seconds, bpm).c_str(), (double)info_.chop[s].seconds);
            setText(P_SEL2, buf);
        }
        else { setText(P_SEL1, "kein Sample"); setText(P_SEL2, ""); }

        // steps and lanes: the bar that plays (bar 1 while stopped)
        int step = curStep_.load();
        int bar = step >= 0 ? step / hh::STEPS : 0;
        setText(P_BARTXT, step >= 0 ? "Takt " + std::to_string(bar + 1) + " / 4" : std::string("Stopp"));
        for (int j = 0; j < kPads; ++j)
        {
            int st = bar * hh::STEPS + j;
            bool now = step >= 0 && step % hh::STEPS == j;
            setFrame(P_STEPLIT0 + j, now ? 1 : 0);
            bool has[kLanes] = {false, rend_.bass[st].pitch >= 0, rend_.mel[st].pitch >= 0, rend_.pad[st].pitch[0] >= 0};
            for (int v = 0; v < hh::DRUMS; ++v) if (rend_.drum[v][st].vel > 0.f) has[0] = true;
            for (int k = 0; k < kLanes; ++k) setFrame(P_CELL0 + k * kPads + j, (has[k] ? 1 : 0) + (now ? 2 : 0));
        }
    }

    // the companions: alive (beat changes), preset name, NEW requests
    void laneTexts()
    {
        genralink::LinkData* L = link_.data();
        auto now = std::chrono::steady_clock::now();
        int alive = 0;
        for (int k = 0; k < kLanes; ++k)
        {
            bool on = false;
            std::string name = "nicht da";
            if (L)
            {
                uint32_t b = L->lane[k].beat.load();
                if (b != laneBeat_[k]) { laneBeat_[k] = b; laneSeen_[k] = now; laneEver_[k] = true; }
                on = laneEver_[k] && now - laneSeen_[k] < std::chrono::milliseconds(1500);
                if (on) { name = genralink::Link::readName(L->lane[k]); if (name.empty()) name = "verbunden"; }
                uint32_t rq = L->lane[k].newRequest.load();
                if (!laneReqInit_[k]) { laneReq_[k] = rq; laneReqInit_[k] = true; }
                if (rq != laneReq_[k]) { laneReq_[k] = rq; if (on) newLane(k); }
            }
            if (on) ++alive;
            setFrame(P_LAMP0 + k, on ? 1 : 0);
            setText(P_LANETXT0 + k, name);
        }
        setText(P_LINKTXT, "LINK · " + std::to_string(alive));
        if (!L) setText(P_LINKINFO, "Fehler: " + link_.error());
        else setText(P_LINKINFO, alive ? std::to_string(alive) + " Plugin(s) verbunden" : std::string("kein Begleit-Plugin"));
    }

    void newLane(int k)
    {
        const char* names[kLanes] = {"Drums", "Bass", "Melodie", "Pads"};
        regenerate(k); // hh::T_DRUMS..T_PAD are 0..3, like the lanes
        setText(P_STATUS, std::string("NEW ") + names[k] + ": ab dem naechsten Takt");
    }

    // ------------------------------------------------------------------ worker: files and settings
    std::string settingsPath() const
    {
        std::string f = mpc::pluginFolder();
        return f.empty() ? std::string() : f + "/settings.txt";
    }
    std::string settingsValue(const std::string& key) const
    {
        std::string t;
        if (!mpc::readTextFile(settingsPath(), t)) return std::string();
        size_t i = t.find(key + "=");
        if (i == std::string::npos) return std::string();
        size_t e = t.find('\n', i);
        size_t a = i + key.size() + 1;
        return t.substr(a, e == std::string::npos ? std::string::npos : e - a);
    }
    void saveSettings()
    {
        std::string p = settingsPath();
        if (p.empty()) return;
        std::string s = "lastdir=" + lastDir_ + "\npadmap=" + padMapString() + "\nsavedir=" + saveDir_ + "\n";
        if (!writeFileBytes(p, s.data(), s.size())) setText(P_WRITE, "Fehler: settings.txt");
    }
    std::string padMapString() const
    {
        if (!mapLearned_.load()) return std::string();
        std::string s;
        for (int k = 0; k < kPads; ++k)
            for (int n = 0; n < 128; ++n)
                if (noteToPad_[n].load() == k) { s += (s.empty() ? "" : ",") + std::to_string(n); break; }
        return s;
    }
    void loadPadMap()
    {
        std::string m = settingsValue("padmap");
        std::vector<int> notes;
        size_t pos = 0;
        while (pos < m.size())
        {
            size_t e = m.find(',', pos);
            notes.push_back(std::atoi(m.substr(pos, e == std::string::npos ? std::string::npos : e - pos).c_str()));
            if (e == std::string::npos) break;
            pos = e + 1;
        }
        if ((int)notes.size() != kPads) return;
        for (int k = 0; k < kPads; ++k) if (notes[(size_t)k] >= 0 && notes[(size_t)k] < 128) noteToPad_[notes[(size_t)k]].store(k);
        mapLearned_.store(true);
    }
    void writeTest()
    {
        std::string folder = mpc::pluginFolder();
        if (folder.empty()) { setText(P_WRITE, "kein Plugin-Ordner"); return; }
        std::string p = folder + "/write_test.txt";
        const char msg[] = "GENRA CHOP write test\n";
        if (!writeFileBytes(p, msg, sizeof msg - 1)) { setText(P_WRITE, "Fehler (Plugin-Ordner)"); return; }
        std::remove(p.c_str());
        setText(P_WRITE, "OK");
    }

    // SAVE WAV: the selected chop, with its pitch
    void saveChop()
    {
        int chop = sel_;
        if (chop < 0 || chop >= info_.count || src_.frames() == 0) { setText(P_STATUS, "erst einen Chop waehlen"); return; }
        std::string dir;
        {
            std::lock_guard<std::mutex> g(mtx_);
            size_t i = loadedPath_.find_last_of('/');
            dir = i == std::string::npos ? mpc::pluginFolder() : loadedPath_.substr(0, i);
        }
        dir += "/GENRA CHOP";
        if (!saveDir_.empty() && isDir(saveDir_)) dir = saveDir_;
        ::mkdir(dir.c_str(), 0777);
        if (chopEnd_[chop] <= chopStart_[chop]) { setText(P_STATUS, "Fehler: Chop leer"); return; }
        std::vector<uint8_t> wav = hh::writeWav(src_, chopStart_[chop], chopEnd_[chop], false, (int)std::lround(cPitch_[chop].load()));
        char name[96];
        std::snprintf(name, sizeof name, "/%s chop%02d.wav", sampleName_.substr(0, 60).c_str(), chop + 1);
        bool ok = writeFileBytes(dir + name, wav.data(), wav.size());
        setText(P_STATUS, ok ? "Chop " + std::to_string(chop + 1) + " als WAV gespeichert" : std::string("Fehler: WAV"));
        setText(P_WRITE, ok ? "OK" : "Fehler: WAV");
    }

    void setupTexts()
    {
        if (learning_.load())
        {
            int step = learnStep_.load();
            setText(P_LEARNINFO, step < kPads ? "Jetzt Pad " + std::to_string(step + 1) + " druecken ..." : std::string("fertig"));
        }
        else
            setText(P_LEARNINFO, mapLearned_.load() ? std::string("gelernt") : std::string("noch nicht gelernt"));
        if (mapLearned_.load())
        {
            int first = -1;
            for (int n = 0; n < 128 && first < 0; ++n) if (noteToPad_[n].load() == 0) first = n;
            setText(P_MAPINFO, "gelernt, Pad 1 = Note " + std::to_string(first));
        }
        else
            setText(P_MAPINFO, "Standard, Pad 1 = Note 36");
        setText(P_SAVEDIRTXT, saveDir_.empty() ? std::string("neben dem Sample (/GENRA CHOP)") : pathLabel(saveDir_), true);
        std::string names;
        for (const std::string& d : findDrives(false)) names += (names.empty() ? "" : ", ") + pathLabel(d);
        setText(P_DRIVES, names.empty() ? std::string("keins gefunden") : names);
    }

    void startLearning()
    {
        for (auto& n : learnNotes_) n.store(-1);
        learnStep_.store(0);
        learning_.store(true, std::memory_order_release);
    }
    void finishLearning()
    {
        learning_.store(false);
        for (auto& n : noteToPad_) n.store(-1);
        for (int k = 0; k < kPads; ++k)
        {
            int n = learnNotes_[k].load();
            if (n >= 0 && n < 128) noteToPad_[n].store(k);
        }
        mapLearned_.store(true);
        saveSettings();
    }

    // ------------------------------------------------------------------ worker: buttons
    void handleButtons()
    {
        for (int i = 0; i < kNumButtons; ++i)
        {
            const int p = kFirstButton + i;
            int v = getInt(p);
            if (v != btnSeen_[i] && v == btnPrev_[i])
            {
                int delta = v - btnSeen_[i];
                btnSeen_[i] = v;
                if (p == P_NEWALL) { regenerate(-1); setText(P_STATUS, "NEW ALL: neuer Loop ab dem naechsten Takt"); }
                else if (p >= P_NEW0 && p < P_NEW0 + kLanes) newLane(p - P_NEW0);
                else if (p == P_UNDO)
                {
                    if (hasUndo_) { std::swap(pat_, undoPat_); publishLoop(); setText(P_STATUS, "UNDO: voriger Loop"); }
                    else setText(P_STATUS, "nichts zum Rueckgaengigmachen");
                }
                else if (p == P_SAVE) saveChop();
                else if (p == P_CHMINUS || p == P_CHPLUS)
                    setParamFromWorker(P_CHOPS, (float)clamp(getInt(P_CHOPS) + (p == P_CHPLUS ? 1 : -1), 4, 64));
                else if (p == P_STPREV || p == P_STNEXT)
                {
                    int s = (getInt(P_STYLE) + (p == P_STNEXT ? 1 : hh::NUM_STYLES - 1)) % hh::NUM_STYLES;
                    setParamFromWorker(P_STYLE, (float)s);
                }
                else if (p == P_LEARN) { startLearning(); setupTexts(); }
                else if (p == P_SCROLL) { top_ += delta > 0 ? 1 + (delta - 1) / 6 : -1 - (-delta - 1) / 6; clampTop(); }
                else if (p == P_BACK) goUp();
                else if (p == P_DRVBTN) { cwd_.clear(); listDirNow(); }
                else if (p == P_PGUP) { top_ -= kRows; clampTop(); }
                else if (p == P_PGDN) { top_ += kRows; clampTop(); }
                else if (p == P_SAVEHERE)
                {
                    if (cwd_.empty() || cwd_ == "*") setText(P_WRITE, "erst einen Ordner oeffnen");
                    else { saveDir_ = cwd_; saveSettings(); setText(P_WRITE, "Speicherort gesetzt"); setupTexts(); }
                }
                else if (p >= P_ROWTAP0 && p < P_ROWTAP0 + kRows) tapRow(p - P_ROWTAP0);
            }
            btnPrev_[i] = v;
        }
    }

    void workerMain()
    {
        using clock = std::chrono::steady_clock;
        writeTest();
        lastDir_ = settingsValue("lastdir");
        saveDir_ = settingsValue("savedir");
        cwd_ = (!lastDir_.empty() && isDir(lastDir_)) ? lastDir_ : std::string();
        listDirNow();
        clock::time_point cutChanged{}, lastSlow{};
        int lastMode = getInt(P_MODE), lastChops = getInt(P_CHOPS), lastStyle = getInt(P_STYLE);
        bool cutPending = false;
        while (!quit_.load())
        {
            auto now = clock::now();
            std::string want;
            bool wantLoad = false;
            {
                std::lock_guard<std::mutex> g(mtx_);
                if (pathRequest_) { want = requestedPath_; pathRequest_ = false; wantLoad = true; }
            }
            if (wantLoad)
            {
                if (!want.empty() && loadPath(want)) cutAndPublish(true);
                else if (restored_) cutAndPublish(false); // the project had the built-in loop: its pattern back
            }
            handleButtons();
            if (bankReset_.exchange(false)) { notifyHostParameterChanged(P_BANK); requestDisplayUpdate(); }
            if (learning_.load() && learnStep_.load() >= kPads) { finishLearning(); setupTexts(); }
            if (lastDirDirty_) { lastDirDirty_ = false; saveSettings(); }

            // selection: from the chop strip, or from the MPC's pads in the CHOP view
            int req = selReq_.exchange(-1);
            if (req >= 0 && req < std::max(1, info_.count) && req != sel_) { sel_ = req; knobsFromChop(); }
            else knobsToChop();

            int m = getInt(P_MODE), c = getInt(P_CHOPS);
            if (m != lastMode || c != lastChops) { lastMode = m; lastChops = c; cutPending = true; cutChanged = now; }
            if (cutPending && now - cutChanged > std::chrono::milliseconds(400)) { cutPending = false; cutAndPublish(false); }
            int st = getInt(P_STYLE);
            if (st != lastStyle) { lastStyle = st; setText(P_STATUS, "STYLE " + styleLabel(st) + ": NEW ALL druecken"); }

            if (now - lastSlow > std::chrono::seconds(3))
            {
                lastSlow = now;
                setupTexts();
                if (cwd_.empty()) listDirNow();
            }
            else if (learning_.load())
                setupTexts();
            browserTexts();
            chopTexts();
            laneTexts();
            displayColumns();
            pushScreen();
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
        }
    }

    // ------------------------------------------------------------------ data
    // audio thread
    hh::ChopSet* cur_ = nullptr;
    Seq* seq_ = nullptr;
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
    std::atomic<int> lastNote_{-1}, lastChop_{-1}, lastPlayed_{-1}, hostBpmX100_{0}, curStep_{-1}, selReq_{-1};
    std::atomic<uint64_t> playingMask_{0};
    std::atomic<float> cPitch_[hh::PADS], cDecay_[hh::PADS], cFilter_[hh::PADS], cLevel_[hh::PADS];
    std::atomic<int8_t> noteToPad_[128];
    std::atomic<bool> mapLearned_{false}, learning_{false};
    std::atomic<int> learnStep_{0};
    std::atomic<int> learnNotes_[kPads];
    // worker
    std::thread worker_;
    std::atomic<bool> quit_{false};
    bool started_ = false;
    genralink::Link link_;
    hh::SampleData src_;
    hh::SampleInfo info_;
    float sampleBpm_ = 0.f;
    size_t chopStart_[hh::PADS] = {}, chopEnd_[hh::PADS] = {};
    std::vector<float> peaks_;
    std::string sampleName_;
    hh::Pattern pat_, undoPat_;
    bool hasUndo_ = false;
    hh::Settings set_;
    hh::Knobs knobs_;
    hh::Render rend_;
    uint32_t seed_ = 1;
    bool loopDirty_ = false;
    int sel_ = 0, knobSel_ = -1;
    float shownKnob_[4] = {};
    uint32_t laneBeat_[kLanes] = {}, laneReq_[kLanes] = {};
    bool laneEver_[kLanes] = {}, laneReqInit_[kLanes] = {};
    std::chrono::steady_clock::time_point laneSeen_[kLanes] = {};
    std::vector<Entry> entries_;
    std::string cwd_, lastDir_, saveDir_;
    std::atomic<bool> bankReset_{false};
    int top_ = 0;
    bool lastDirDirty_ = false;
    int btnSeen_[kNumButtons] = {}, btnPrev_[kNumButtons] = {};
    std::vector<std::string> shown_;
    std::vector<int> frame_, frameShown_;
    // shared between worker, UI thread and state (mtx_)
    std::mutex mtx_;
    std::vector<std::string> texts_;
    std::string loadedPath_, requestedPath_;
    bool pathRequest_ = false, restored_ = false;
    hh::Pattern savedPattern_, restoredPattern_;
};

MPC_PLUGIN_ENTRY(GenraChop)
