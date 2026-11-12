// CHOP TEST - the mini test for GENRA CHOP on the MPC.
//
// SPDX-License-Identifier: GPL-3.0-only
//
// Three hand-made screen pages (built by chop-test/tools/build_skin.py):
//   BROWSER  path, ten rows to tap (folder: open, WAV: load), ZURUECK, LAUFWERKE, page buttons, LISTE (data wheel)
//   PADS     an info bar, the 16 pads of the current bank (number, chord, lit while playing, tap to play),
//            MODUS / CHOPS / PITCH / VOLUME, banks A-D, CHOP SPEICHERN, "Zuletzt: ..."
//   SETUP    LERNEN (teach the plugin which notes the MPC's pads send) and the technical status
//
// What it checks on the device: drives and WAVs, the GENRA CHOP engine (src/Chop.cpp, Core.cpp, Styles.cpp, copied
// from Camillowup/genra-chop) on the MPC's CPU, writing files, sharing data with CHOP LINK (common/chop_link.h) and
// live screen updates.
//
// Signal flow: MIDI note -> pad (learned map, or note - 36) -> chop (pad + 16 x bank) -> one of 8 voices
//              (linear interpolation, short fades, same chop chokes itself) -> VOLUME -> out 1/2.
//
// Threads: process() (audio) only plays what the worker published and reads/writes atomics. A worker thread
// (started in prepare()) browses, loads and cuts samples, writes files and makes the screen texts. The MPC only
// re-reads a text when it thinks the value changed, so the worker nudges every changed text parameter and tells the
// host (audioMasterAutomate + audioMasterUpdateDisplay). formatValue() (UI thread) reads the texts under a mutex.

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

static const int kRows = 10;   // rows of the browser
static const int kPads = 16;   // pads per bank

enum
{
    // controls
    P_VOLUME = 0,                 // master volume (parameter 0: the host nudges it on load, harmless)
    P_MODE,                       // MODUS: Transient / Equal
    P_CHOPS,                      // 4..64
    P_PITCH,                      // all chops, semitones
    P_BANK,                       // A-D: which 16 chops the pads play
    P_SAVE,                       // button CHOP SPEICHERN (every tap flips it; a change is a tap)
    P_LEARN,                      // button LERNEN
    P_SCROLL,                     // LISTE: endless, one wheel click = one row
    P_BACK, P_DRVBTN, P_PGUP, P_PGDN, // browser buttons
    P_PADTAP0,                    // 16 buttons: the pads on the screen
    P_ROWTAP0 = P_PADTAP0 + kPads,// 10 buttons: the browser rows
    // texts (read only)
    P_PATH = P_ROWTAP0 + kRows,
    P_LISTINFO,                   // "Zeile 3-12 von 24"
    P_LOADED,                     // the sample's name
    P_INFOBAR,                    // "F#m  ·  92 BPM  ·  16 Chops  ·  bereit"
    P_LAST,                       // "Zuletzt: Pad 5, Chop 5, F#m, Note 40"
    P_PADNUM0,                    // 16: the chop number on each pad ("5")
    P_PADCHORD0 = P_PADNUM0 + kPads,   // 16: its chord
    P_PADLIT0 = P_PADCHORD0 + kPads,   // 16: 1 while it plays (film strip frame)
    P_ROWT0 = P_PADLIT0 + kPads,       // 10: the browser rows
    P_LEARNINFO = P_ROWT0 + kRows,
    P_MAPINFO,
    P_DRIVES,
    P_WRITE,
    P_LINK,
    P_ANALYSE,
    P_KEYTXT,                     // info bar, split (texts are cut after 31 characters): "Tonart F#m"
    P_TEMPOTXT,                   // "Sample 92 / MPC 92 BPM"
    P_CHOPTXT,                    // "16 Chops, Bank A"
    P_COUNT
};

static const int kVoices = 8;
static const int kFadeIn = 32;
static const int kFadeOut = 96;

struct Voice
{
    bool active = false;
    int chop = -1;
    double pos = 0.0, end = 0.0, rate = 1.0;
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
        addButton("save", "CHOP SPEICHERN");
        addButton("learn", "LERNEN");
        addParam(intParam("scroll", "LISTE", 0, 1000, 500));
        addButton("back", "ZURUECK");
        addButton("drvbtn", "LAUFWERKE");
        addButton("pgup", "SEITE ZURUECK");
        addButton("pgdn", "SEITE VOR");
        for (int i = 0; i < kPads; ++i) addButton(("padtap" + std::to_string(i + 1)).c_str(), ("PAD " + std::to_string(i + 1)).c_str());
        for (int i = 0; i < kRows; ++i) addButton(("rowtap" + std::to_string(i + 1)).c_str(), ("TIPP " + std::to_string(i + 1)).c_str());
        addText("path", "PFAD");
        addText("listinfo", "LISTINFO");
        addText("loaded", "GELADEN");
        addText("infobar", "INFO");
        addText("last", "ZULETZT");
        for (int i = 0; i < kPads; ++i) addText(("padnum" + std::to_string(i + 1)).c_str(), ("NUMMER " + std::to_string(i + 1)).c_str());
        for (int i = 0; i < kPads; ++i) addText(("padchord" + std::to_string(i + 1)).c_str(), ("AKKORD " + std::to_string(i + 1)).c_str());
        for (int i = 0; i < kPads; ++i) addText(("padlit" + std::to_string(i + 1)).c_str(), ("LICHT " + std::to_string(i + 1)).c_str());
        for (int i = 0; i < kRows; ++i) addText(("rowt" + std::to_string(i + 1)).c_str(), ("ZEILE " + std::to_string(i + 1)).c_str());
        addText("learninfo", "LERNINFO");
        addText("mapinfo", "BELEGUNG");
        addText("drives", "LAUFWERKSLISTE");
        addText("write", "SCHREIBEN");
        addText("link", "CHOP LINK");
        addText("analyse", "ANALYSE");
        addText("keytxt", "TONART");
        addText("tempotxt", "TEMPO");
        addText("choptxt", "CHOPINFO");
        texts_.assign(P_COUNT, std::string());
        shown_.assign(P_COUNT, std::string("\x01"));
        for (auto& r : retired_) r.store(nullptr);
        for (auto& n : noteToPad_) n.store(-1);
        resyncButtons();
    }

    ~ChopTest() override
    {
        quit_.store(true);
        if (worker_.joinable()) worker_.join();
        delete cur_;
        delete pending_.exchange(nullptr);
        for (auto& r : retired_) delete r.exchange(nullptr);
    }

    static PluginInfo makeInfo()
    {
        PluginInfo i;
        i.name = "CHOP TEST";
        i.vendor = "Genra";
        i.uniqueId = MPC_FOURCC('G', 'n', 'C', 't');
        i.version = 5000;
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
            loadPadMap();
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
        if (hh::ChopSet* n = pending_.exchange(nullptr, std::memory_order_acq_rel))
        {
            for (Voice& v : voices_) v.active = false;
            if (cur_) retire(cur_);
            cur_ = n;
        }

        choplink::LinkData* L = link_.data();
        if (L)
        {
            L->writerBeat.fetch_add(1, std::memory_order_relaxed);
            if (ctx.transport.tempoValid) L->bpmX100.store((int32_t)std::lround(ctx.transport.bpm * 100.0), std::memory_order_relaxed);
        }
        if (ctx.transport.tempoValid) hostBpmX100_.store((int)std::lround(ctx.transport.bpm * 100.0), std::memory_order_relaxed);

        const float pitch = (float)getInt(P_PITCH);
        const int bank = clamp(getInt(P_BANK), 0, 3);

        // pads tapped on the screen (button parameters flip on every tap)
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
                    size_t k = (size_t)v.pos;
                    if (v.pos >= v.end - 1.0 || k + 1 >= dl.size()) { v.active = false; continue; }
                    float f = (float)(v.pos - (double)k);
                    float a = dl[k] + (dl[k + 1] - dl[k]) * f;
                    float b = dr[k] + (dr[k + 1] - dr[k]) * f;
                    float env = v.gain;
                    if (v.age < kFadeIn) env *= (float)v.age / kFadeIn;
                    double left = (v.end - v.pos) / v.rate;
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
            outL[i] = l * gainSmooth_;
            if (outR) outR[i] = r * gainSmooth_;
        }
        uint64_t mask = 0;
        for (const Voice& v : voices_) if (v.active && v.chop >= 0 && v.chop < 64) mask |= (uint64_t)1 << v.chop;
        playingMask_.store(mask, std::memory_order_relaxed);
    }

    // ------------------------------------------------------------------ state: the sample's path
    void saveExtraState(std::vector<uint8_t>& out) override
    {
        std::lock_guard<std::mutex> g(mtx_);
        std::string s = "v1\n" + loadedPath_;   // never empty, so loadExtraState always runs
        out.assign(s.begin(), s.end());
    }

    void loadExtraState(const uint8_t* data, size_t size) override
    {
        std::lock_guard<std::mutex> g(mtx_);
        std::string s(reinterpret_cast<const char*>(data), size);
        if (s.compare(0, 3, "v1\n") == 0) s = s.substr(3);
        requestedPath_ = s;
        pathRequest_ = !s.empty();
        resyncButtons(); // the buttons were set by the project: that is no tap
    }

    // ------------------------------------------------------------------ screen texts (UI thread)
    bool formatValue(int index, float, std::string& out) override
    {
        if (index == P_SCROLL) { std::lock_guard<std::mutex> g(mtx_); out = texts_[P_LISTINFO]; return true; }
        if (index < P_PATH) return false;
        std::lock_guard<std::mutex> g(mtx_);
        out = texts_[(size_t)index];
        return true;
    }

private:
    // ------------------------------------------------------------------ parameters
    void addText(const char* id, const char* name)
    {
        Param p = floatParam(id, name, 0.f, 1.f, 0.f);
        p.automatable = false;
        addParam(p);
    }
    void addButton(const char* id, const char* name) { addParam(boolParam(id, name, false)); }

    static const int kButtons = P_PATH - P_SAVE; // every button and LISTE: changes are checked by the worker
    void resyncButtons()
    {
        for (int i = 0; i < kButtons; ++i) btnSeen_[i] = btnPrev_[i] = getInt(P_SAVE + i);
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
            else lastChop_.store(-2, std::memory_order_relaxed); // a note that plays nothing
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
        for (Voice& v : voices_) if (v.active && v.chop == chop && v.release < 0) v.release = kFadeOut / 2;
        Voice* slot = nullptr;
        for (Voice& v : voices_) if (!v.active) { slot = &v; break; }
        if (!slot)
        {
            slot = &voices_[0];
            for (Voice& v : voices_) if (v.age > slot->age) slot = &v;
        }
        slot->active = true;
        slot->chop = chop;
        slot->pos = (double)cur_->start[chop];
        slot->end = (double)cur_->end[chop];
        slot->rate = (double)cur_->data.sampleRate / sr_ * std::pow(2.0, pitch / 12.0);
        slot->gain = 0.6f * (0.2f + 0.8f * vel);
        slot->age = 0;
        slot->release = -1;
        hitCount_.fetch_add(1, std::memory_order_relaxed);
        if (choplink::LinkData* L = link_.data())
        {
            L->lastPad.store(chop, std::memory_order_relaxed);
            L->trigCount.fetch_add(1, std::memory_order_release);
        }
    }

    void retire(hh::ChopSet* s)
    {
        for (auto& r : retired_)
        {
            hh::ChopSet* expected = nullptr;
            if (r.compare_exchange_strong(expected, s)) return;
        }
    }

    // ------------------------------------------------------------------ worker: texts
    // The SDK hands the MPC at most 31 characters per text: cut longer ones (paths keep their end).
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

    // The MPC re-reads a text only when it thinks the value changed: nudge every changed text parameter, tell the host,
    // and ask for a redraw. The PADLIT parameters carry real values (0/1: the frame of the pad's film strip).
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
            if (i >= P_PADLIT0 && i < P_PADLIT0 + kPads) setNormalised(i, now == "1" ? 1.f : 0.f);
            else setNormalised(i, getNormalised(i) > 0.5f ? 0.f : 1.f);
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
    }

    void rememberDir() { if (cwd_ != "*") { lastDir_ = cwd_; lastDirDirty_ = true; } }

    void tapRow(int row)
    {
        int i = top_ + row;
        if (i < 0 || i >= (int)entries_.size()) return;
        const Entry e = entries_[(size_t)i];
        if (e.kind == 0) goUp();
        else if (e.kind == 1 || e.kind == 3) { cwd_ = e.path; listDirNow(); rememberDir(); }
        else if (loadPath(e.path)) cutAndPublish();
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

    // ------------------------------------------------------------------ worker: sample, cut, files
    bool loadPath(const std::string& path)
    {
        setText(P_ANALYSE, "laedt ...");
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
        for (auto& r : retired_) delete r.exchange(nullptr);
        delete pending_.exchange(cs, std::memory_order_acq_rel);

        char buf[64];
        std::snprintf(buf, sizeof buf, "OK, %d Chops in %.2f s", info_.count, secs);
        setText(P_ANALYSE, buf);
        sampleName_ = src_.name;
        ready_ = true;

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

    void writeTest()
    {
        std::string folder = mpc::pluginFolder();
        if (folder.empty()) { setText(P_WRITE, "kein Plugin-Ordner"); return; }
        std::string p = folder + "/write_test.txt";
        const char msg[] = "CHOP TEST write test\n";
        if (!writeFileBytes(p, msg, sizeof msg - 1)) { setText(P_WRITE, "Fehler (Plugin-Ordner)"); return; }
        std::string back;
        if (!mpc::readTextFile(p, back) || back.compare(0, 10, "CHOP TEST ") != 0) { setText(P_WRITE, "Fehler beim Lesen"); return; }
        std::remove(p.c_str());
        setText(P_WRITE, "OK");
    }

    // settings.txt (in the plugin's folder, for every project): the browser's last folder and the learned pad notes
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
        std::string s = "lastdir=" + lastDir_ + "\npadmap=" + padMapString() + "\n";
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

    void saveChop()
    {
        int chop = lastPlayed_.load();
        if (chop < 0 || chop >= info_.count || src_.frames() == 0) { setText(P_WRITE, "erst ein Pad spielen"); return; }
        std::string dir;
        {
            std::lock_guard<std::mutex> g(mtx_);
            size_t i = loadedPath_.find_last_of('/');
            dir = i == std::string::npos ? mpc::pluginFolder() : loadedPath_.substr(0, i);
        }
        dir += "/CHOP TEST";
        ::mkdir(dir.c_str(), 0777);
        if (chopEnd_[chop] <= chopStart_[chop]) { setText(P_WRITE, "Fehler: Chop leer"); return; }
        std::vector<uint8_t> wav = hh::writeWav(src_, chopStart_[chop], chopEnd_[chop], false, getInt(P_PITCH));
        char name[96];
        std::snprintf(name, sizeof name, "/%s chop%02d.wav", sampleName_.substr(0, 60).c_str(), chop + 1);
        bool ok = writeFileBytes(dir + name, wav.data(), wav.size());
        setText(P_WRITE, ok ? "Chop " + std::to_string(chop + 1) + " gespeichert" : std::string("Fehler: WAV"));
    }

    // ------------------------------------------------------------------ worker: the PADS and SETUP texts
    void padTexts()
    {
        const int bank = clamp(getInt(P_BANK), 0, 3);
        const uint64_t playing = playingMask_.load();
        for (int k = 0; k < kPads; ++k)
        {
            int chop = k + kPads * bank;
            setText(P_PADNUM0 + k, chop < info_.count ? std::to_string(chop + 1) : std::string("-"));
            setText(P_PADCHORD0 + k, chordText(info_, chop));
            setText(P_PADLIT0 + k, ((playing >> chop) & 1u) ? "1" : "0");
        }
        char k[16];
        hh::keyName(info_.key, k, sizeof k);
        int host = hostBpmX100_.load();
        char buf[64];
        setText(P_KEYTXT, std::string("Tonart ") + k);
        std::snprintf(buf, sizeof buf, "Sample %.0f / MPC %.0f BPM", (double)sampleBpm_, host / 100.0);
        setText(P_TEMPOTXT, buf);
        std::snprintf(buf, sizeof buf, "%d Chops, Bank %c", info_.count, "ABCD"[bank]);
        setText(P_CHOPTXT, buf);
        setText(P_LOADED, sampleName_.empty() ? std::string("Testloop (eingebaut)") : sampleName_);

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
            setText(P_LINK, alive ? "hoert zu" : "nicht geladen");
        }
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

    // a change that has stayed for two looks (>= 40 ms) is a tap / a turn; the MPC's short probes are not
    void handleButtons()
    {
        for (int i = 0; i < kButtons; ++i)
        {
            const int p = P_SAVE + i;
            int v = getInt(p);
            if (v != btnSeen_[i] && v == btnPrev_[i])
            {
                int delta = v - btnSeen_[i];
                btnSeen_[i] = v;
                if (p == P_SAVE) saveChop();
                else if (p == P_LEARN) { startLearning(); setupTexts(); }
                else if (p == P_SCROLL) { top_ += delta > 0 ? 1 + (delta - 1) / 6 : -1 - (-delta - 1) / 6; clampTop(); }
                else if (p == P_BACK) goUp();
                else if (p == P_DRVBTN) { cwd_.clear(); listDirNow(); }
                else if (p == P_PGUP) { top_ -= kRows; clampTop(); }
                else if (p == P_PGDN) { top_ += kRows; clampTop(); }
                else if (p >= P_ROWTAP0 && p < P_ROWTAP0 + kRows) tapRow(p - P_ROWTAP0);
                // P_PADTAP0..: handled in process() (no delay)
            }
            btnPrev_[i] = v;
        }
    }

    void workerMain()
    {
        using clock = std::chrono::steady_clock;
        writeTest();
        lastDir_ = settingsValue("lastdir");
        cwd_ = (!lastDir_.empty() && isDir(lastDir_)) ? lastDir_ : std::string();
        listDirNow();
        clock::time_point cutChanged{}, lastSlow{};
        int lastMode = getInt(P_MODE), lastChops = getInt(P_CHOPS);
        bool cutPending = false;

        while (!quit_.load())
        {
            auto now = clock::now();

            // a project brought its sample along
            std::string want;
            {
                std::lock_guard<std::mutex> g(mtx_);
                if (pathRequest_) { want = requestedPath_; pathRequest_ = false; }
            }
            if (!want.empty() && loadPath(want)) cutAndPublish();

            handleButtons();
            if (learning_.load() && learnStep_.load() >= kPads) { finishLearning(); setupTexts(); }
            if (lastDirDirty_) { lastDirDirty_ = false; saveSettings(); }

            // MODUS / CHOPS: cut again once the knob rests
            int m = getInt(P_MODE), c = getInt(P_CHOPS);
            if (m != lastMode || c != lastChops) { lastMode = m; lastChops = c; cutPending = true; cutChanged = now; }
            if (cutPending && now - cutChanged > std::chrono::milliseconds(300)) { cutPending = false; cutAndPublish(); }

            // the drive list follows USB sticks / SD cards
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
            pushScreen();
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
        }
    }

    // ------------------------------------------------------------------ data
    // audio thread
    hh::ChopSet* cur_ = nullptr;
    Voice voices_[kVoices];
    double sr_ = 44100.0;
    float gainSmooth_ = -1.f;
    int padTapSeen_[kPads] = {};

    // shared with the audio thread (lock-free)
    std::atomic<hh::ChopSet*> pending_{nullptr};
    std::atomic<hh::ChopSet*> retired_[4];
    std::atomic<int> lastPad_{-1}, lastNote_{-1}, lastChop_{-1}, lastPlayed_{-1}, hostBpmX100_{0};
    std::atomic<uint32_t> hitCount_{0};
    std::atomic<uint64_t> playingMask_{0};
    std::atomic<int8_t> noteToPad_[128];
    std::atomic<bool> mapLearned_{false}, learning_{false};
    std::atomic<int> learnStep_{0};
    std::atomic<int> learnNotes_[kPads];

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
    std::string sampleName_;
    bool ready_ = false;
    uint32_t lastReaderBeat_ = 0;
    std::chrono::steady_clock::time_point readerSeen_{};
    bool readerEver_ = false;
    std::vector<Entry> entries_;
    std::string cwd_, lastDir_;   // "" = the drive list, "*" = every mount
    int top_ = 0;                 // first row shown
    bool lastDirDirty_ = false;
    int btnSeen_[kButtons] = {}, btnPrev_[kButtons] = {};
    std::vector<std::string> shown_; // what the MPC was last told about, per text parameter

    // shared between worker and UI thread (mtx_)
    std::mutex mtx_;
    std::vector<std::string> texts_;
    std::string loadedPath_, requestedPath_;
    bool pathRequest_ = false;
};

MPC_PLUGIN_ENTRY(ChopTest)
