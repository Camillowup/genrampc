// CHOP TEST - the mini test for GENRA CHOP on the MPC.
//
// SPDX-License-Identifier: GPL-3.0-only
//
// Three hand-made screen pages (built by chop-test/tools/build_skin.py), all operated by tapping:
//   BROWSER  path, ten rows (folder: open; WAV: first tap = listen, second tap = load), ZURÜCK, LAUFWERKE,
//            page buttons, SPEICHERORT HIER, LISTE (data wheel)
//   PADS     LCD bar, a waveform of 64 bars with the chop borders, the 16 pads of the current bank (number, chord,
//            REV / pitch / LOCK tags, lit while playing, tap to play), MODUS / CHOPS / PITCH / VOLUME, banks A-D,
//            CHOP SPEICHERN, CHOKE, and the edit row for the last played chop (REV, PITCH -/+, LOCK)
//   SETUP    LERNEN (which notes the MPC's pads send) and the technical status
//
// Signal flow: MIDI note -> pad (learned map, or note - 36) -> chop (pad + 16 x bank) -> one of 8 voices
//              (forward or reverse, chop pitch + PITCH, linear interpolation, short fades; CHOKE: one mute group)
//              + the browser's preview voice -> VOLUME -> out 1/2.
//
// Threads: process() (audio) plays what the worker published and reads/writes atomics only. A worker thread (started
// in prepare()) browses, loads and cuts samples, writes files and makes the screen. The MPC re-reads a text or value
// only when it thinks the value changed, so the worker nudges every changed text parameter (and sets the value
// parameters: pad lights, waveform bars, button flashes) and tells the host (audioMasterAutomate +
// audioMasterUpdateDisplay). Fixed texts in a skin are not drawn on the MPC, so every caption is a parameter too.
//
// Settings (last folder, learned pads, save folder) live in <drive>/Genra/CHOP TEST.txt next to "VST2 Plugins",
// so replacing the plugin folder on an update keeps them.

#include "mpc_plugin.h"
#include "Chop.hpp"
#include "../../common/chop_link.h"

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

static const int kRows = 10;    // rows of the browser
static const int kPads = 16;    // pads per bank
static const int kChops = 64;   // chops at most (4 banks)
static const int kBars = 64;    // waveform bars
static const int kHeights = 12; // waveform bar heights (film strip: heights x {normal, playing} x {no border, border})
static const int kBarFrames = kHeights * 4;

// the buttons that flash when tapped (film strip with 2 frames bound to a BLITZ parameter)
enum Flash { FL_BACK, FL_DRIVES, FL_PGUP, FL_PGDN, FL_SAVEHERE, FL_SAVE, FL_CHOKE, FL_LEARN, FL_REV, FL_PDOWN, FL_PUP, FL_LOCK,
             FL_ROW0, FL_COUNT = FL_ROW0 + kRows };

// captions: key (parameter "CAP <key>", build_skin.py finds it by that name) and text
static const char* const kCaptions[][2] = {
    {"back", "ZURÜCK"}, {"drives", "LAUFWERKE"}, {"pgup", "< SEITE"}, {"pgdn", "SEITE >"},
    {"savehere", "SPEICHERORT HIER"}, {"loaded", "Geladen:"},
    {"bankA", "A"}, {"bankB", "B"}, {"bankC", "C"}, {"bankD", "D"},
    {"save", "CHOP SPEICHERN"}, {"learn", "LERNEN"},
    {"help1", "LERNEN antippen, dann die"}, {"help2", "Pads 1 bis 16 der Reihe nach"},
    {"help3", "drücken (Pad 1 = unten links)"},
    {"r1", "Pad-Belegung"}, {"r2", "Laufwerke"}, {"r3", "Schreiben"}, {"r4", "CHOP LINK"}, {"r5", "Analyse"},
    {"r6", "Speicherort"},
    {"rev", "REV"}, {"pdown", "PITCH -"}, {"pup", "PITCH +"}, {"lock", "LOCK"}};
static const int kNumCaptions = (int)(sizeof(kCaptions) / sizeof(kCaptions[0]));

enum
{
    // controls the host owns (knobs) and buttons (every tap flips the value; a change that stays is a tap)
    P_VOLUME = 0,                 // master volume (parameter 0: the host nudges it on load, harmless)
    P_MODE,                       // MODUS: Transient / Equal
    P_CHOPS,                      // 4..64
    P_PITCH,                      // all chops, semitones
    P_BANK,                       // A-D: which 16 chops the pads play
    P_CHOKE,                      // on = all chops in one mute group
    P_SAVE, P_LEARN, P_SCROLL, P_BACK, P_DRVBTN, P_PGUP, P_PGDN, P_SAVEHERE, // buttons (P_SCROLL: endless)
    P_EDREV, P_EDPDOWN, P_EDPUP, P_EDLOCK,                                    // edit row
    P_PADTAP0,                    // 16 buttons: the pads on the screen (played at once, in process())
    P_ROWTAP0 = P_PADTAP0 + kPads,// 10 buttons: the browser rows
    // read-only texts
    P_PATH = P_ROWTAP0 + kRows,
    P_LISTINFO, P_LOADED, P_KEYTXT, P_TEMPOTXT, P_CHOPTXT, P_LAST, P_CHOKETXT, P_EDTXT,
    P_LEARNINFO, P_MAPINFO, P_DRIVES, P_WRITE, P_LINK, P_ANALYSE, P_SAVEDIRTXT,
    P_PADNUM0,                         // 16: the chop number on each pad
    P_PADCHORD0 = P_PADNUM0 + kPads,   // 16: its chord
    P_PADTAG0 = P_PADCHORD0 + kPads,   // 16: "REV +3 L"
    P_ROWT0 = P_PADTAG0 + kPads,       // 10: the browser rows
    P_CAP0 = P_ROWT0 + kRows,          // the captions
    // read-only values (film strip frames)
    P_PADLIT0 = P_CAP0 + kNumCaptions, // 16: 1 while the chop plays
    P_WAVE0 = P_PADLIT0 + kPads,       // 64: waveform bars (frame 0 .. kBarFrames-1)
    P_FLASH0 = P_WAVE0 + kBars,        // the buttons' flashes (frame 0 / 1)
    P_COUNT = P_FLASH0 + FL_COUNT
};
static const int P_FIRST_BUTTON = P_SAVE, P_LAST_BUTTON = P_ROWTAP0 + kRows - 1;
static const int kButtons = P_LAST_BUTTON - P_FIRST_BUTTON + 1;

static const int kVoices = 8;
static const int kFadeIn = 32;
static const int kFadeOut = 96;

struct Voice
{
    bool active = false;
    int chop = -1;
    double pos = 0.0, start = 0.0, end = 0.0, rate = 1.0; // rate < 0: reverse (pos runs from end down to start)
    float gain = 1.f;
    int age = 0;          // samples since start (fade in)
    int release = -1;     // >= 0: fading out, samples left
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

// The SDK hands the MPC at most 31 bytes per text: cut longer ones on a character boundary (UTF-8),
// paths keep their end.
std::string fit(const std::string& s, bool keepEnd = false)
{
    if (s.size() <= 31) return s;
    if (keepEnd)
    {
        size_t from = s.size() - 28;
        while (from < s.size() && ((unsigned char)s[from] & 0xC0) == 0x80) ++from;
        return "..." + s.substr(from);
    }
    size_t n = 30;
    while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) --n;
    return s.substr(0, n) + "~";
}

} // namespace

class ChopTest : public Plugin
{
public:
    ChopTest() : Plugin(makeInfo())
    {
        addParam(floatParam("volume", "VOLUME", -40.f, 6.f, 0.f, "dB", 1));
        addParam(choiceParam("mode", "MODUS", {"Transient", "Equal"}, 0));
        addParam(intParam("chops", "CHOPS", 4, 64, 16));
        addParam(intParam("pitch", "PITCH", -12, 12, 0, "st"));
        addParam(choiceParam("bank", "BANK", {"A", "B", "C", "D"}, 0));
        addParam(boolParam("choke", "CHOKE", true));
        addButton("save", "CHOP SPEICHERN");
        addButton("learn", "LERNEN");
        addParam(intParam("scroll", "LISTE", 0, 1000, 500));
        addButton("back", "ZURUECK");
        addButton("drvbtn", "LAUFWERKE");
        addButton("pgup", "SEITE ZURUECK");
        addButton("pgdn", "SEITE VOR");
        addButton("savehere", "SPEICHERORT HIER");
        addButton("edrev", "ED REV");
        addButton("edpdown", "ED PITCH AB");
        addButton("edpup", "ED PITCH AUF");
        addButton("edlock", "ED LOCK");
        for (int i = 0; i < kPads; ++i) addButton(("padtap" + std::to_string(i + 1)).c_str(), ("PAD " + std::to_string(i + 1)).c_str());
        for (int i = 0; i < kRows; ++i) addButton(("rowtap" + std::to_string(i + 1)).c_str(), ("TIPP " + std::to_string(i + 1)).c_str());
        for (const char* t : {"PFAD", "LISTINFO", "GELADEN", "TONART", "TEMPO", "CHOPINFO", "ZULETZT", "CHOKETEXT", "EDITTEXT",
                              "LERNINFO", "BELEGUNG", "LAUFWERKSLISTE", "SCHREIBEN", "CHOP LINK", "ANALYSE", "SPEICHERORT"})
            addText(t);
        for (int i = 0; i < kPads; ++i) addText(("NUMMER " + std::to_string(i + 1)).c_str());
        for (int i = 0; i < kPads; ++i) addText(("AKKORD " + std::to_string(i + 1)).c_str());
        for (int i = 0; i < kPads; ++i) addText(("MARKEN " + std::to_string(i + 1)).c_str());
        for (int i = 0; i < kRows; ++i) addText(("ZEILE " + std::to_string(i + 1)).c_str());
        for (int i = 0; i < kNumCaptions; ++i) addText((std::string("CAP ") + kCaptions[i][0]).c_str());
        for (int i = 0; i < kPads; ++i) addText(("LICHT " + std::to_string(i + 1)).c_str());
        for (int i = 0; i < kBars; ++i) addText(("WELLE " + std::to_string(i + 1)).c_str());
        for (int i = 0; i < FL_COUNT; ++i) addText(("BLITZ " + std::to_string(i + 1)).c_str());
        texts_.assign(P_COUNT, std::string());
        shown_.assign(P_COUNT, std::string("\x01"));
        for (int i = 0; i < kNumCaptions; ++i) texts_[(size_t)(P_CAP0 + i)] = kCaptions[i][1];
        for (auto& r : retired_) r.store(nullptr);
        for (auto& r : previewRetired_) r.store(nullptr);
        for (auto& n : noteToPad_) n.store(-1);
        for (int c = 0; c < kChops; ++c) { chopRev_[c].store(false); chopPitch_[c].store(0); chopLock_[c].store(false); }
        resyncButtons();
    }

    ~ChopTest() override
    {
        quit_.store(true);
        if (worker_.joinable()) worker_.join();
        delete cur_;
        delete pending_.exchange(nullptr);
        for (auto& r : retired_) delete r.exchange(nullptr);
        delete preview_;
        delete previewPending_.exchange(nullptr);
        for (auto& r : previewRetired_) delete r.exchange(nullptr);
    }

    static PluginInfo makeInfo()
    {
        PluginInfo i;
        i.name = "CHOP TEST";
        i.vendor = "Genra";
        i.uniqueId = MPC_FOURCC('G', 'n', 'C', 't');
        i.version = 7000;
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
            linkOk_ = link_.open();
            loadSettings();
            set(P_BANK, 0.f);           // always start on bank A
            bankReset_.store(true);
            src_ = makeBuiltinLoop();   // the pads sound from the first block on
            cutAndPublish();
            worker_ = std::thread([this] { workerMain(); });
        }
    }

    void reset() override
    {
        for (Voice& v : voices_) v.active = false;
        gainSmooth_ = -1.f;
        for (int k = 0; k < kPads; ++k) padTapSeen_[k] = getInt(P_PADTAP0 + k);
    }

    // ------------------------------------------------------------------ audio
    void process(ProcessContext& ctx) override
    {
        audioStarted_.store(true, std::memory_order_relaxed);
        if (hh::ChopSet* n = pending_.exchange(nullptr, std::memory_order_acq_rel))
        {
            for (Voice& v : voices_) v.active = false;
            if (cur_) retireSet(cur_);
            cur_ = n;
        }
        if (hh::SampleData* p = previewPending_.exchange(nullptr, std::memory_order_acq_rel))
        {
            if (preview_) retirePreview(preview_);
            preview_ = p;
            previewPos_ = 0.0;
            previewFade_ = -1;
        }
        if (previewStop_.exchange(false) && preview_) previewFade_ = kFadeOut * 4;

        choplink::LinkData* L = link_.data();
        if (L)
        {
            L->writerBeat.fetch_add(1, std::memory_order_relaxed);
            if (ctx.transport.tempoValid) L->bpmX100.store((int32_t)std::lround(ctx.transport.bpm * 100.0), std::memory_order_relaxed);
        }
        if (ctx.transport.tempoValid) hostBpmX100_.store((int)std::lround(ctx.transport.bpm * 100.0), std::memory_order_relaxed);

        const float pitch = (float)getInt(P_PITCH);
        const int bank = clamp(getInt(P_BANK), 0, 3);

        // pads tapped on the screen (button parameters flip on every tap): played at once
        for (int k = 0; k < kPads; ++k)
        {
            int v = getInt(P_PADTAP0 + k);
            if (v != padTapSeen_[k])
            {
                padTapSeen_[k] = v;
                lastNote_.store(-1, std::memory_order_relaxed);
                playPad(k, bank, 0.8f, pitch);
            }
        }

        float target = std::pow(10.f, get(P_VOLUME) / 20.f);
        if (gainSmooth_ < 0.f) gainSmooth_ = target;
        float* outL = ctx.outputs[0];
        float* outR = ctx.numOutputs > 1 ? ctx.outputs[1] : nullptr;
        const double previewRate = preview_ ? (double)preview_->sampleRate / sr_ : 1.0;
        int ev = 0;
        for (int i = 0; i < ctx.numFrames; ++i)
        {
            while (ev < ctx.numMidiEvents && ctx.midi[ev].sampleOffset <= i)
            {
                const MidiEvent& e = ctx.midi[ev++];
                if (e.isNoteOn()) noteOn(e.note(), e.velocity() / 127.f, bank, pitch);
                else if (e.isAllNotesOff())
                    for (Voice& v : voices_) if (v.active && v.release < 0) v.release = kFadeOut;
            }
            gainSmooth_ += (target - gainSmooth_) * 0.002f;
            float l = 0.f, r = 0.f;
            if (cur_)
            {
                const std::vector<float>& dl = cur_->data.L;
                const std::vector<float>& dr = cur_->data.R.empty() ? cur_->data.L : cur_->data.R;
                for (Voice& v : voices_)
                {
                    if (!v.active) continue;
                    const bool fwd = v.rate > 0.0;
                    if (fwd ? v.pos >= v.end - 1.0 : v.pos <= v.start) { v.active = false; continue; }
                    size_t k = (size_t)v.pos;
                    if (k + 1 >= dl.size()) { v.active = false; continue; }
                    float f = (float)(v.pos - (double)k);
                    float a = dl[k] + (dl[k + 1] - dl[k]) * f;
                    float b = dr[k] + (dr[k + 1] - dr[k]) * f;
                    float env = v.gain;
                    if (v.age < kFadeIn) env *= (float)v.age / kFadeIn;
                    double left = (fwd ? v.end - v.pos : v.pos - v.start) / std::fabs(v.rate);
                    if (left < kFadeOut) env *= (float)(left / kFadeOut);
                    if (v.release >= 0)
                    {
                        env *= (float)v.release / kFadeOut;
                        if (--v.release <= 0) { v.active = false; continue; }
                    }
                    l += a * env; r += b * env;
                    v.pos += v.rate;
                    ++v.age;
                }
            }
            if (preview_)
            {
                size_t k = (size_t)previewPos_;
                const std::vector<float>& pl = preview_->L;
                const std::vector<float>& pr = preview_->R.empty() ? preview_->L : preview_->R;
                if (k + 1 < pl.size() && previewFade_ != 0)
                {
                    float f = (float)(previewPos_ - (double)k);
                    float g = 0.5f;
                    if (previewFade_ > 0) { g *= (float)previewFade_ / (kFadeOut * 4); --previewFade_; }
                    l += (pl[k] + (pl[k + 1] - pl[k]) * f) * g;
                    r += (pr[k] + (pr[k + 1] - pr[k]) * f) * g;
                    previewPos_ += previewRate;
                }
            }
            outL[i] = l * gainSmooth_;
            if (outR) outR[i] = r * gainSmooth_;
        }
        uint64_t mask = 0;
        for (const Voice& v : voices_) if (v.active && v.chop >= 0 && v.chop < 64) mask |= (uint64_t)1 << v.chop;
        playingMask_.store(mask, std::memory_order_relaxed);
    }

    // ------------------------------------------------------------------ state: the sample's path, the chop edits
    void saveExtraState(std::vector<uint8_t>& out) override
    {
        std::lock_guard<std::mutex> g(mtx_);
        std::string s = "v2\n" + loadedPath_ + "\n";   // never empty, so loadExtraState always runs
        for (int c = 0; c < kChops; ++c)
            s += std::to_string((chopRev_[c].load() ? 1 : 0) | (chopLock_[c].load() ? 2 : 0)) + "," + std::to_string(chopPitch_[c].load()) + ";";
        out.assign(s.begin(), s.end());
    }

    void loadExtraState(const uint8_t* data, size_t size) override
    {
        std::lock_guard<std::mutex> g(mtx_);
        std::string s(reinterpret_cast<const char*>(data), size);
        std::string path, edits;
        if (s.compare(0, 3, "v1\n") == 0) path = s.substr(3);
        else if (s.compare(0, 3, "v2\n") == 0)
        {
            size_t e = s.find('\n', 3);
            path = s.substr(3, e == std::string::npos ? std::string::npos : e - 3);
            if (e != std::string::npos) edits = s.substr(e + 1);
        }
        for (int c = 0; c < kChops; ++c) { chopRev_[c].store(false); chopLock_[c].store(false); chopPitch_[c].store(0); }
        size_t pos = 0;
        for (int c = 0; c < kChops && pos < edits.size(); ++c)
        {
            size_t semi = edits.find(';', pos);
            std::string one = edits.substr(pos, semi == std::string::npos ? std::string::npos : semi - pos);
            size_t comma = one.find(',');
            if (comma != std::string::npos)
            {
                int flags = std::atoi(one.substr(0, comma).c_str());
                chopRev_[c].store((flags & 1) != 0);
                chopLock_[c].store((flags & 2) != 0);
                chopPitch_[c].store(clamp(std::atoi(one.substr(comma + 1).c_str()), -12, 12));
            }
            if (semi == std::string::npos) break;
            pos = semi + 1;
        }
        requestedPath_ = path;
        pathRequest_ = !path.empty();
        if (!audioStarted_.load())   // opening a project (before any audio): start on bank A too;
        {                            // a state loaded while playing keeps its bank
            set(P_BANK, 0.f);
            bankReset_.store(true);
        }
        resyncButtons();   // the buttons were set by the project: that is no tap
    }

    // ------------------------------------------------------------------ screen texts (UI thread)
    bool formatValue(int index, float, std::string& out) override
    {
        if (index == P_SCROLL) { std::lock_guard<std::mutex> g(mtx_); out = listShort_; return true; }
        if (index < P_PATH) return false;
        std::lock_guard<std::mutex> g(mtx_);
        out = texts_[(size_t)index];
        return true;
    }

private:
    // ------------------------------------------------------------------ parameters
    void addText(const char* name)
    {
        std::string id = name;
        for (char& ch : id) ch = ch == ' ' ? '_' : (char)::tolower((unsigned char)ch);
        Param p = floatParam(id.c_str(), name, 0.f, 1.f, 0.f);
        p.automatable = false;
        addParam(p);
    }
    void addButton(const char* id, const char* name) { addParam(boolParam(id, name, false)); }

    void resyncButtons()
    {
        for (int i = 0; i < kButtons; ++i) btnSeen_[i] = btnPrev_[i] = getInt(P_FIRST_BUTTON + i);
    }

    // ------------------------------------------------------------------ audio helpers
    // MIDI note -> pad: the learned map, else note - 36 (pads 1..64 without banks)
    void noteOn(int note, float vel, int bank, float pitch)
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
            return; // no sound while learning
        }
        int pad = noteToPad_[note & 127].load(std::memory_order_relaxed);
        if (pad >= 0) { playPad(pad, bank, vel, pitch); return; }
        if (!mapLearned_.load(std::memory_order_relaxed))
        {
            int chop = note - 36;
            if (chop >= 0 && chop < 64) startVoice(chop, vel, pitch, chop % kPads);
            else lastChop_.store(-2, std::memory_order_relaxed);
        }
        else
            lastChop_.store(-2, std::memory_order_relaxed);
    }

    void playPad(int pad, int bank, float vel, float pitch) { startVoice(pad + kPads * bank, vel, pitch, pad); }

    void startVoice(int chop, float vel, float pitch, int pad)
    {
        lastPad_.store(pad, std::memory_order_relaxed);
        if (!cur_ || chop >= cur_->info.count) { lastChop_.store(-1 - 1000 - chop, std::memory_order_relaxed); return; }
        lastChop_.store(chop, std::memory_order_relaxed);
        lastPlayed_.store(chop, std::memory_order_relaxed);
        // CHOKE on: one mute group (every chop cuts the others); off: only the same chop cuts itself
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
        const double semis = pitch + chopPitch_[chop].load(std::memory_order_relaxed);
        const double rate = (double)cur_->data.sampleRate / sr_ * std::pow(2.0, semis / 12.0);
        const bool rev = chopRev_[chop].load(std::memory_order_relaxed);
        slot->active = true;
        slot->chop = chop;
        slot->start = (double)cur_->start[chop];
        slot->end = (double)cur_->end[chop];
        slot->pos = rev ? slot->end - 2.0 : slot->start;
        slot->rate = rev ? -rate : rate;
        slot->gain = 0.6f * (0.2f + 0.8f * vel);
        slot->age = 0;
        slot->release = -1;
        if (choplink::LinkData* L = link_.data())
        {
            L->lastPad.store(chop, std::memory_order_relaxed);
            L->trigCount.fetch_add(1, std::memory_order_release);
        }
    }

    void retireSet(hh::ChopSet* s)
    {
        for (auto& r : retired_)
        {
            hh::ChopSet* expected = nullptr;
            if (r.compare_exchange_strong(expected, s)) return;
        }
    }
    void retirePreview(hh::SampleData* s)
    {
        for (auto& r : previewRetired_)
        {
            hh::SampleData* expected = nullptr;
            if (r.compare_exchange_strong(expected, s)) return;
        }
    }

    // ------------------------------------------------------------------ worker: texts
    void setText(int index, const std::string& s, bool keepEnd = false)
    {
        std::lock_guard<std::mutex> g(mtx_);
        texts_[(size_t)index] = fit(s, keepEnd);
    }
    void setValue(int index, int frame) { setText(index, std::to_string(frame)); }

    // A message for a moment (2 s) in place of a line's usual text.
    void flashMessage(int index, const std::string& s)
    {
        message_[index == P_LISTINFO ? 0 : 1] = s;
        messageUntil_[index == P_LISTINFO ? 0 : 1] = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    }
    bool messageActive(int which) const { return std::chrono::steady_clock::now() < messageUntil_[which]; }

    void flash(int f) { flashUntil_[f] = std::chrono::steady_clock::now() + std::chrono::milliseconds(350); }

    // The MPC re-reads a text or value only when it thinks the value changed: nudge every changed text parameter
    // (value parameters get their real value: the film strip frame), tell the host, ask for a redraw.
    void pushScreen()
    {
        bool any = false;
        for (int i = P_PATH; i < P_COUNT; ++i)
        {
            std::string now;
            {
                std::lock_guard<std::mutex> g(mtx_);
                now = texts_[(size_t)i];
            }
            if (now == shown_[(size_t)i]) continue;
            shown_[(size_t)i] = now;
            if (i >= P_PADLIT0)
            {
                int frames = (i >= P_WAVE0 && i < P_WAVE0 + kBars) ? kBarFrames : 2;
                setNormalised(i, clamp((float)std::atoi(now.c_str()) / (float)(frames - 1), 0.f, 1.f));
            }
            else
                setNormalised(i, getNormalised(i) > 0.5f ? 0.f : 1.f);
            notifyHostParameterChanged(i);
            any = true;
        }
        if (any) requestDisplayUpdate();
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
        stopPreview();
    }

    void rememberDir() { if (cwd_ != "*") { lastDir_ = cwd_; settingsDirty_ = true; } }

    // a WAV: first tap = listen (preview), second tap on the same file = load
    void tapRow(int row)
    {
        int i = top_ + row;
        if (i < 0 || i >= (int)entries_.size()) return;
        const Entry e = entries_[(size_t)i];
        if (e.kind == 0) goUp();
        else if (e.kind == 1 || e.kind == 3) { cwd_ = e.path; listDirNow(); rememberDir(); stopPreview(); }
        else if (e.path == previewPath_)
        {
            stopPreview();
            if (loadPath(e.path)) { cutAndPublish(); flashMessage(P_LISTINFO, "geladen: " + stripWav(e.label)); }
        }
        else startPreview(e.path, e.label);
    }

    void startPreview(const std::string& path, const std::string& label)
    {
        std::vector<uint8_t> bytes;
        hh::SampleData* d = new hh::SampleData();
        std::string err;
        if (!mpc::readFile(path, bytes) || !hh::readWav(bytes, *d, err, 30.f))
        {
            delete d;
            flashMessage(P_LISTINFO, "nicht lesbar: " + label);
            return;
        }
        for (auto& r : previewRetired_) delete r.exchange(nullptr);
        delete previewPending_.exchange(d, std::memory_order_acq_rel);
        previewPath_ = path;
        flashMessage(P_LISTINFO, "nochmal tippen = laden");
    }

    void stopPreview()
    {
        if (previewPath_.empty()) return;
        previewPath_.clear();
        previewStop_.store(true);
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
                if (e.kind == 0) t = "..  (zurück)";
                else if (e.kind == 1 || e.kind == 3) t = "[" + e.label + "]";
                else if (!previewPath_.empty() && e.path == previewPath_) t = "» " + e.label;
                else t = ((!loaded.empty() && e.path == loaded) ? "> " : "") + e.label;
            }
            setText(P_ROWT0 + r, t);
        }
        setText(P_PATH, cwd_.empty() ? std::string("Laufwerke") : pathLabel(cwd_), true);
        {
            char sh[32];
            std::snprintf(sh, sizeof sh, "%d/%d", entries_.empty() ? 0 : top_ + 1, (int)entries_.size());
            std::lock_guard<std::mutex> g(mtx_);
            listShort_ = sh;
        }
        if (messageActive(0)) setText(P_LISTINFO, message_[0]);
        else
        {
            char b[64];
            int n = (int)entries_.size();
            if (n == 0) std::snprintf(b, sizeof b, "leer");
            else std::snprintf(b, sizeof b, "Zeile %d-%d von %d", top_ + 1, std::min(n, top_ + kRows), n);
            setText(P_LISTINFO, b);
        }
    }

    // ------------------------------------------------------------------ worker: sample, cut, files
    bool loadPath(const std::string& path)
    {
        setText(P_ANALYSE, "lädt ...");
        std::vector<uint8_t> bytes;
        if (!mpc::readFile(path, bytes)) { setText(P_ANALYSE, "Fehler: nicht lesbar"); return false; }
        hh::SampleData d;
        std::string err;
        if (!hh::readWav(bytes, d, err, 60.f)) { setText(P_ANALYSE, "Fehler: " + err); return false; }
        d.path = path;
        d.name = stripWav(baseName(path));
        src_ = std::move(d);
        std::lock_guard<std::mutex> g(mtx_);
        loadedPath_ = path;
        return true;
    }

    void cutAndPublish()
    {
        if (src_.frames() == 0) return;
        auto t0 = std::chrono::steady_clock::now();
        hh::ChopSet* cs = new hh::ChopSet();
        cs->data = src_;
        hh::analyse(*cs, getInt(P_MODE) == 1 ? hh::CM_EQUAL : hh::CM_TRANSIENT, clamp(getInt(P_CHOPS), 4, 64));
        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

        info_ = cs->info;
        sampleBpm_ = cs->bpm;
        for (int p = 0; p < 64; ++p)
        {
            chopStart_[p] = p < cs->info.count ? cs->start[p] : 0;
            chopEnd_[p] = p < cs->info.count ? cs->end[p] : 0;
        }
        // the waveform: 64 bars (the loudest point of each 64th), heights relative to the loudest bar
        const size_t n = cs->data.frames();
        float top = 1e-6f;
        for (int b = 0; b < kBars; ++b)
        {
            size_t a = n * (size_t)b / kBars, e = n * (size_t)(b + 1) / kBars;
            float pk = 0.f;
            for (size_t i = a; i < e; ++i) pk = std::max(pk, std::fabs(cs->data.L[i]));
            barPeak_[b] = pk;
            top = std::max(top, pk);
            barChop_[b] = -1;
            barBorder_[b] = false;
            for (int c = 0; c < cs->info.count; ++c)
            {
                size_t mid = (a + e) / 2;
                if (mid >= cs->start[c] && mid < cs->end[c]) barChop_[b] = c;
                if (c > 0 && cs->start[c] >= a && cs->start[c] < e) barBorder_[b] = true;
            }
        }
        for (int b = 0; b < kBars; ++b) barPeak_[b] /= top;

        for (auto& r : retired_) delete r.exchange(nullptr);
        delete pending_.exchange(cs, std::memory_order_acq_rel);

        char buf[64];
        std::snprintf(buf, sizeof buf, "OK, %d Chops in %.2f s", info_.count, secs);
        setText(P_ANALYSE, buf);
        sampleName_ = src_.name;

        if (choplink::LinkData* L = link_.data())
        {
            L->keyRoot.store(info_.key.root);
            L->keyMinor.store(info_.key.minor ? 1 : 0);
            L->chopCount.store(info_.count);
            for (int p = 0; p < choplink::kPads; ++p)
            {
                hh::Chord c = hh::padChord(info_, nullptr, p);
                L->chordRoot[p].store((int8_t)(p < info_.count && c.valid() ? c.root : -1));
                L->chordQuality[p].store((int8_t)(p < info_.count && c.valid() ? c.quality : -1));
            }
            link_.writeName(sampleName_);
        }
    }

    // settings: <drive>/Genra/CHOP TEST.txt (the folder above "VST2 Plugins"), so updates of the plugin keep them.
    // Fallback: settings.txt in the plugin's own folder (also read once from there, where older versions wrote it).
    std::string settingsPath() const
    {
        std::string f = mpc::pluginFolder();
        if (f.empty()) return std::string();
        size_t a = f.find_last_of('/');
        std::string vst2 = a == std::string::npos ? std::string() : f.substr(0, a);
        size_t b = vst2.find_last_of('/');
        if (b != std::string::npos && baseName(vst2) == "VST2 Plugins")
        {
            std::string dir = vst2.substr(0, b) + "/Genra";
            ::mkdir(dir.c_str(), 0777);
            if (isDir(dir)) return dir + "/CHOP TEST.txt";
        }
        return f + "/settings.txt";
    }

    static std::string valueOf(const std::string& t, const std::string& key)
    {
        size_t i = t.find(key + "=");
        if (i == std::string::npos) return std::string();
        size_t e = t.find('\n', i);
        size_t a = i + key.size() + 1;
        return t.substr(a, e == std::string::npos ? std::string::npos : e - a);
    }

    void loadSettings()
    {
        std::string t;
        if (!mpc::readTextFile(settingsPath(), t)) mpc::readTextFile(mpc::pluginFolder() + "/settings.txt", t);
        lastDir_ = valueOf(t, "lastdir");
        saveDir_ = valueOf(t, "savedir");
        std::string m = valueOf(t, "padmap");
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

    void saveSettings()
    {
        std::string p = settingsPath();
        if (p.empty()) return;
        std::string s = "lastdir=" + lastDir_ + "\npadmap=" + padMapString() + "\nsavedir=" + saveDir_ + "\n";
        if (!writeFileBytes(p, s.data(), s.size())) setText(P_WRITE, "Fehler: Einstellungen");
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

    void writeTest()
    {
        std::string p = settingsPath();
        if (p.empty()) { setText(P_WRITE, "kein Plugin-Ordner"); return; }
        std::string t = p + ".test";
        const char msg[] = "CHOP TEST write test\n";
        if (!writeFileBytes(t, msg, sizeof msg - 1)) { setText(P_WRITE, "Fehler beim Schreiben"); return; }
        std::remove(t.c_str());
        setText(P_WRITE, "OK");
    }

    void saveChop()
    {
        int chop = lastPlayed_.load();
        if (chop < 0 || chop >= info_.count || src_.frames() == 0) { flashMessage(P_LAST, "erst ein Pad spielen"); return; }
        std::string dir;
        {
            std::lock_guard<std::mutex> g(mtx_);
            size_t i = loadedPath_.find_last_of('/');
            dir = (i == std::string::npos ? mpc::pluginFolder() : loadedPath_.substr(0, i)) + "/CHOP TEST";
        }
        if (!saveDir_.empty() && isDir(saveDir_)) dir = saveDir_;  // SPEICHERORT HIER
        ::mkdir(dir.c_str(), 0777);
        if (chopEnd_[chop] <= chopStart_[chop]) { flashMessage(P_LAST, "Chop ist leer"); return; }
        std::vector<uint8_t> wav = hh::writeWav(src_, chopStart_[chop], chopEnd_[chop], chopRev_[chop].load(),
                                                getInt(P_PITCH) + chopPitch_[chop].load());
        char name[96];
        std::snprintf(name, sizeof name, "/%s chop%02d.wav", sampleName_.substr(0, 60).c_str(), chop + 1);
        bool ok = writeFileBytes(dir + name, wav.data(), wav.size());
        flashMessage(P_LAST, ok ? "Chop " + std::to_string(chop + 1) + " gespeichert: " + pathLabel(dir) : std::string("Fehler beim Speichern"));
        setText(P_WRITE, ok ? "OK, Chop gespeichert" : std::string("Fehler: WAV"));
    }

    // ------------------------------------------------------------------ worker: the PADS and SETUP screens
    void padTexts()
    {
        const int bank = clamp(getInt(P_BANK), 0, 3);
        const uint64_t playing = playingMask_.load();
        for (int k = 0; k < kPads; ++k)
        {
            int chop = k + kPads * bank;
            setText(P_PADNUM0 + k, chop < info_.count ? std::to_string(chop + 1) : std::string("-"));
            setText(P_PADCHORD0 + k, chordText(info_, chop));
            std::string tag;
            if (chop < info_.count)
            {
                if (chopRev_[chop].load()) tag += "REV ";
                int pt = chopPitch_[chop].load();
                if (pt) tag += (pt > 0 ? "+" : "") + std::to_string(pt) + " ";
                if (chopLock_[chop].load()) tag += "L";
            }
            setText(P_PADTAG0 + k, tag);
            setValue(P_PADLIT0 + k, ((playing >> chop) & 1u) ? 1 : 0);
        }
        // waveform: height + 12 x playing + 24 x border
        for (int b = 0; b < kBars; ++b)
        {
            int h = clamp((int)std::lround(barPeak_[b] * (kHeights - 1)), 0, kHeights - 1);
            int c = barChop_[b];
            bool on = c >= 0 && ((playing >> c) & 1u);
            setValue(P_WAVE0 + b, h + (on ? kHeights : 0) + (barBorder_[b] ? 2 * kHeights : 0));
        }
        char k[16];
        hh::keyName(info_.key, k, sizeof k);
        char buf[64];
        setText(P_KEYTXT, std::string("Tonart ") + k);
        std::snprintf(buf, sizeof buf, "Sample %.0f / MPC %.0f BPM", (double)sampleBpm_, hostBpmX100_.load() / 100.0);
        setText(P_TEMPOTXT, buf);
        std::snprintf(buf, sizeof buf, "%d Chops, Bank %c", info_.count, "ABCD"[bank]);
        setText(P_CHOPTXT, buf);
        setText(P_CHOKETXT, getBool(P_CHOKE) ? "CHOKE AN" : "CHOKE AUS");
        setText(P_LOADED, sampleName_.empty() ? std::string("Testloop (eingebaut)") : sampleName_);

        // the edit row: the last played chop
        int ed = lastPlayed_.load();
        if (ed < 0 || ed >= info_.count) setText(P_EDTXT, "Chop: erst ein Pad spielen");
        else
        {
            int pt = chopPitch_[ed].load();
            setText(P_EDTXT, "Chop " + std::to_string(ed + 1) + (chopRev_[ed].load() ? "  REV" : "") + "  Pitch "
                             + (pt > 0 ? "+" : "") + std::to_string(pt) + (chopLock_[ed].load() ? "  LOCK" : ""));
        }

        if (messageActive(1)) { setText(P_LAST, message_[1]); return; }
        int note = lastNote_.load(), pad = lastPad_.load(), chop = lastChop_.load();
        std::string last;
        if (note < 0 && pad < 0) last = "noch nichts gespielt";
        else
        {
            std::string from = note >= 0 ? "Note " + std::to_string(note) : std::string("Touch");
            if (chop >= 0) last = from + " > Pad " + std::to_string(pad + 1) + " = Chop " + std::to_string(chop + 1) + " " + chordText(info_, chop);
            else if (chop <= -1000) last = from + " > Chop " + std::to_string(-chop - 1000) + " fehlt";
            else last = from + " > kein Pad (LERNEN)";
        }
        setText(P_LAST, last);
    }

    void setupTexts()
    {
        if (learning_.load())
        {
            int step = learnStep_.load();
            setText(P_LEARNINFO, step < kPads ? "Jetzt Pad " + std::to_string(step + 1) + " drücken ..." : std::string("fertig"));
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
        setText(P_SAVEDIRTXT, saveDir_.empty() ? std::string("neben dem Sample (/CHOP TEST)") : pathLabel(saveDir_), true);

        std::string names;
        for (const std::string& d : findDrives(false)) names += (names.empty() ? "" : ", ") + pathLabel(d);
        setText(P_DRIVES, names.empty() ? std::string("keins gefunden") : names);

        choplink::LinkData* L = link_.data();
        if (!L) setText(P_LINK, "Fehler: " + link_.error());
        else
        {
            uint32_t rb = L->readerBeat.load();
            auto now = std::chrono::steady_clock::now();
            if (rb != lastReaderBeat_) { lastReaderBeat_ = rb; readerSeen_ = now; readerEver_ = true; }
            bool alive = readerEver_ && now - readerSeen_ < std::chrono::milliseconds(1500);
            setText(P_LINK, alive ? "hört zu" : "nicht geladen");
        }
    }

    void flashTexts()
    {
        auto now = std::chrono::steady_clock::now();
        for (int f = 0; f < FL_COUNT; ++f) setValue(P_FLASH0 + f, now < flashUntil_[f] ? 1 : 0);
    }

    // ------------------------------------------------------------------ worker: buttons
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

    void editChop(int what)
    {
        int c = lastPlayed_.load();
        if (c < 0 || c >= info_.count) { flashMessage(P_LAST, "erst ein Pad spielen"); return; }
        if (what == 0) chopRev_[c].store(!chopRev_[c].load());
        else if (what == 1) chopPitch_[c].store(std::max(-12, chopPitch_[c].load() - 1));
        else if (what == 2) chopPitch_[c].store(std::min(12, chopPitch_[c].load() + 1));
        else chopLock_[c].store(!chopLock_[c].load());
    }

    // a change that has stayed for two looks (>= 40 ms) is a tap / a turn; the MPC's short probes are not
    void handleButtons()
    {
        for (int i = 0; i < kButtons; ++i)
        {
            const int p = P_FIRST_BUTTON + i;
            int v = getInt(p);
            if (v != btnSeen_[i] && v == btnPrev_[i])
            {
                int delta = v - btnSeen_[i];
                btnSeen_[i] = v;
                switch (p)
                {
                    case P_SAVE: flash(FL_SAVE); saveChop(); break;
                    case P_LEARN: flash(FL_LEARN); startLearning(); setupTexts(); break;
                    case P_SCROLL: top_ += delta > 0 ? 1 + (delta - 1) / 6 : -1 - (-delta - 1) / 6; clampTop(); break;
                    case P_BACK: flash(FL_BACK); goUp(); break;
                    case P_DRVBTN: flash(FL_DRIVES); cwd_.clear(); listDirNow(); stopPreview(); break;
                    case P_PGUP: flash(FL_PGUP); top_ -= kRows; clampTop(); break;
                    case P_PGDN: flash(FL_PGDN); top_ += kRows; clampTop(); break;
                    case P_SAVEHERE:
                        flash(FL_SAVEHERE);
                        if (cwd_.empty() || cwd_ == "*") flashMessage(P_LISTINFO, "erst einen Ordner öffnen");
                        else
                        {
                            saveDir_ = cwd_;
                            settingsDirty_ = true;
                            flashMessage(P_LISTINFO, "Speicherort: " + pathLabel(saveDir_));
                            setupTexts();
                        }
                        break;
                    case P_EDREV: flash(FL_REV); editChop(0); break;
                    case P_EDPDOWN: flash(FL_PDOWN); editChop(1); break;
                    case P_EDPUP: flash(FL_PUP); editChop(2); break;
                    case P_EDLOCK: flash(FL_LOCK); editChop(3); break;
                    default:
                        if (p >= P_ROWTAP0 && p < P_ROWTAP0 + kRows) { flash(FL_ROW0 + p - P_ROWTAP0); tapRow(p - P_ROWTAP0); }
                        // P_PADTAP0..: played in process() (no delay)
                        break;
                }
            }
            btnPrev_[i] = v;
        }
        // CHOKE is a plain on/off the audio thread reads; it still flashes when it changes
        bool ck = getBool(P_CHOKE);
        if (ck != chokeSeen_) { chokeSeen_ = ck; flash(FL_CHOKE); }
    }

    void workerMain()
    {
        using clock = std::chrono::steady_clock;
        writeTest();
        chokeSeen_ = getBool(P_CHOKE);
        cwd_ = (!lastDir_.empty() && isDir(lastDir_)) ? lastDir_ : std::string();
        listDirNow();
        clock::time_point cutChanged{}, lastSlow{};
        int lastMode = getInt(P_MODE), lastChops = getInt(P_CHOPS);
        bool cutPending = false;

        while (!quit_.load())
        {
            auto now = clock::now();
            std::string want;
            {
                std::lock_guard<std::mutex> g(mtx_);
                if (pathRequest_) { want = requestedPath_; pathRequest_ = false; }
            }
            if (!want.empty() && loadPath(want)) cutAndPublish();

            handleButtons();
            if (bankReset_.exchange(false)) { notifyHostParameterChanged(P_BANK); requestDisplayUpdate(); }
            if (learning_.load() && learnStep_.load() >= kPads) { finishLearning(); setupTexts(); }
            if (settingsDirty_) { settingsDirty_ = false; saveSettings(); }
            for (auto& r : previewRetired_) delete r.exchange(nullptr);

            int m = getInt(P_MODE), c = getInt(P_CHOPS);
            if (m != lastMode || c != lastChops) { lastMode = m; lastChops = c; cutPending = true; cutChanged = now; }
            if (cutPending && now - cutChanged > std::chrono::milliseconds(300)) { cutPending = false; cutAndPublish(); }

            if (now - lastSlow > std::chrono::seconds(3))
            {
                lastSlow = now;
                setupTexts();
                if (cwd_.empty()) listDirNow();
            }
            else if (learning_.load())
                setupTexts();

            browserTexts();
            padTexts();
            flashTexts();
            pushScreen();
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
        }
    }

    // ------------------------------------------------------------------ data
    // audio thread
    hh::ChopSet* cur_ = nullptr;
    hh::SampleData* preview_ = nullptr;
    double previewPos_ = 0.0;
    int previewFade_ = -1;   // > 0: fading out; 0: silent
    Voice voices_[kVoices];
    double sr_ = 44100.0;
    float gainSmooth_ = -1.f;
    int padTapSeen_[kPads] = {};

    // shared with the audio thread (lock-free)
    std::atomic<hh::ChopSet*> pending_{nullptr};
    std::atomic<hh::ChopSet*> retired_[4];
    std::atomic<hh::SampleData*> previewPending_{nullptr};
    std::atomic<hh::SampleData*> previewRetired_[4];
    std::atomic<bool> previewStop_{false};
    std::atomic<int> lastPad_{-1}, lastNote_{-1}, lastChop_{-1}, lastPlayed_{-1}, hostBpmX100_{0};
    std::atomic<uint64_t> playingMask_{0};
    std::atomic<int8_t> noteToPad_[128];
    std::atomic<bool> mapLearned_{false}, learning_{false};
    std::atomic<int> learnStep_{0};
    std::atomic<int> learnNotes_[kPads];
    std::atomic<bool> chopRev_[kChops], chopLock_[kChops];
    std::atomic<int> chopPitch_[kChops];
    std::atomic<bool> bankReset_{false}, audioStarted_{false};

    // worker
    std::thread worker_;
    std::atomic<bool> quit_{false};
    bool started_ = false;
    choplink::Link link_;
    bool linkOk_ = false;
    hh::SampleData src_;
    hh::SampleInfo info_;
    float sampleBpm_ = 0.f;
    size_t chopStart_[64] = {}, chopEnd_[64] = {};
    float barPeak_[kBars] = {};
    int barChop_[kBars] = {};
    bool barBorder_[kBars] = {};
    std::string sampleName_;
    uint32_t lastReaderBeat_ = 0;
    std::chrono::steady_clock::time_point readerSeen_{};
    bool readerEver_ = false;
    bool chokeSeen_ = true;
    std::vector<Entry> entries_;
    std::string cwd_, lastDir_;   // "" = the drive list, "*" = every mount
    std::string saveDir_;         // SPEICHERORT HIER ("" = next to the sample)
    std::string previewPath_;     // the WAV that plays as a preview ("" = none)
    int top_ = 0;                 // first row shown
    bool settingsDirty_ = false;
    int btnSeen_[kButtons] = {}, btnPrev_[kButtons] = {};
    std::vector<std::string> shown_; // what the MPC was last told about, per text parameter
    std::chrono::steady_clock::time_point flashUntil_[FL_COUNT] = {};
    std::string message_[2];      // 0: browser line, 1: pads line
    std::chrono::steady_clock::time_point messageUntil_[2] = {};

    // shared between worker and UI thread (mtx_)
    std::mutex mtx_;
    std::vector<std::string> texts_;
    std::string loadedPath_, requestedPath_, listShort_;
    bool pathRequest_ = false;
};

MPC_PLUGIN_ENTRY(ChopTest)
