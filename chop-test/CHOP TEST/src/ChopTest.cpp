// CHOP TEST - the mini test for GENRA CHOP on the MPC.
//
// SPDX-License-Identifier: GPL-3.0-only
//
// What it checks on the device:
//   * the drives (every mount in /proc/mounts under /nvme, /media, /mnt, ...) can be browsed and WAVs read
//   * the GENRA CHOP engine (src/Chop.cpp, Core.cpp, Styles.cpp: copied from Camillowup/genra-chop) cuts and
//     analyses a sample on the MPC's CPU (key, tempo, a chord per chop) and how long that takes
//   * the plugin may write files (settings.txt in its own folder, a chop as WAV next to the sample)
//   * two plugins can share data (common/chop_link.h): CHOP LINK on another track follows the pads
//   * live values on screen (page WAVE: one bar per chop of the current bank) with two update modes
//
// Signal flow: MIDI note -> pad (note - PAD NOTE) -> one of 8 voices plays that chop of the sample
//              (linear interpolation, short fades, same pad chokes itself) -> VOLUME -> out 1/2.
//
// Threads: process() (audio) only plays what the worker published. A worker thread (started in prepare())
// scans folders, loads and cuts samples, writes files and prepares the texts the screen shows.
// formatValue() (UI thread) reads those texts under a mutex; process() never locks.

#include "mpc_plugin.h"
#include "Chop.hpp"
#include "../../common/chop_link.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <dirent.h>
#include <mutex>
#include <sys/stat.h>
#include <thread>

using namespace mpc;

namespace {

enum
{
    P_VOLUME = 0, // master volume (parameter 0: the host nudges it on load, harmless)
    P_PATH,       // read-only: the folder the browser is in
    P_BROWSE,     // endless: every step moves the browser one entry (drives, "..", folders, WAVs)
    P_OPEN,       // endless: every turn opens the entry under BROWSE (folder: go in, WAV: load)
    P_LOADED,     // read-only: the loaded sample
    P_MODE,       // Transient / Equal
    P_CHOPS,      // 4..64 chops
    P_PITCH,      // all chops in semitones
    P_KEY,        // read-only texts from here ...
    P_TEMPO,
    P_PAD,
    P_ANALYSE,
    P_DRIVES,
    P_WRITE,
    P_LINK,
    P_SAVE,       // endless: every turn saves the last played chop as WAV
    P_UPDATE,     // how the screen learns about new values: Text (polling) / Notify (host notified)
    P_PADNOTE,    // MIDI note of pad 1
    P_CHORD0,     // 16 read-only: chords of the current bank
    P_WAVE0 = P_CHORD0 + 16, // 16 read-only bars: level of each chop of the current bank
    P_DIAG = P_WAVE0 + 16, // read-only: how often the MPC asked about BROWSE/OPEN with another value vs. real turns
    // the BROWSER page (a hand-written skin page: rows to tap, buttons)
    P_ROWT0,                  // 10 read-only: the names in the rows
    P_ROWB0 = P_ROWT0 + 10,   // 10 buttons: tapping a row (every tap flips the value; any change counts)
    P_BACK = P_ROWB0 + 10,    // button ZURUECK
    P_DRVBTN,                 // button LAUFWERKE
    P_PGUP,                   // button < SEITE
    P_PGDN,                   // button SEITE >
    P_PAGEINFO,               // read-only: "Seite 1/3, 24 Eintraege"
    P_COUNT
};
static const int kRows = 10;

static const int kVoices = 8;
static const int kFadeIn = 32;
static const int kFadeOut = 96;
static const int kPadNotes[] = {36, 37, 48, 60};

struct Voice
{
    bool active = false;
    int pad = -1;
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

// Entries of a folder: sub folders or .wav files (no hidden entries).
std::vector<std::string> listDir(const std::string& dir, bool wantDirs)
{
    std::vector<std::string> out;
    DIR* d = ::opendir(dir.c_str());
    if (!d) return out;
    while (dirent* e = ::readdir(d))
    {
        std::string n = e->d_name;
        if (n.empty() || n[0] == '.' || n[0] == '$') continue; // hidden, $RECYCLE.BIN ...
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

// A built-in test loop (2 bars at 92 BPM, F#m7 / Bm9 stabs with a kick and hats), so the pads sound even
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
    const float chords[2][4] = {{54, 57, 61, 64}, {59, 62, 66, 69}}; // F#m7, Bm7 (+9 below)
    uint32_t rng = 12345;
    for (int b = 0; b < 8; ++b)
    {
        size_t at = (size_t)b * beat;
        // kick on 1 and 3 of each bar
        if (b % 2 == 0)
            for (size_t i = 0; i < beat / 2 && at + i < n; ++i)
            {
                float t = i / sr;
                float v = 0.9f * std::sin(2.f * 3.14159265f * (50.f + 80.f * std::exp(-t * 30.f)) * t) * std::exp(-t * 9.f);
                s.L[at + i] += v; s.R[at + i] += v;
            }
        // chord stab on every beat
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
        // hats on the off beats
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

} // namespace

class ChopTest : public Plugin
{
public:
    ChopTest() : Plugin(makeInfo())
    {
        addParam(floatParam("volume", "VOLUME", -40.f, 6.f, 0.f, "dB", 1));
        addText("path", "PATH");
        addParam(intParam("browse", "BROWSE", 0, 1000, 500));
        addParam(intParam("open", "OPEN", 0, 1000, 500));
        addText("loaded", "GELADEN");
        addParam(choiceParam("mode", "MODE", {"Transient", "Equal"}, 0));
        addParam(intParam("chops", "CHOPS", 4, 64, 16));
        addParam(intParam("pitch", "PITCH", -12, 12, 0, "st"));
        addText("key", "KEY");
        addText("tempo", "TEMPO");
        addText("pad", "PAD");
        addText("analyse", "ANALYSE");
        addText("drives", "LAUFWERKE");
        addText("write", "SCHREIBEN");
        addText("link", "LINK");
        addParam(intParam("save", "SAVE CHOP", 0, 1000, 500));
        addParam(choiceParam("update", "UPDATE", {"Text", "Notify"}, 0));
        addParam(choiceParam("padnote", "PAD NOTE", {"36 (C1)", "37 (C#1)", "48 (C2)", "60 (C3)"}, 0));
        for (int i = 0; i < 16; ++i)
        {
            char id[16], nm[16];
            std::snprintf(id, sizeof id, "chord%d", i + 1);
            std::snprintf(nm, sizeof nm, "AKKORD %d", i + 1);
            addText(id, nm);
        }
        for (int i = 0; i < 16; ++i)
        {
            char id[16], nm[16];
            std::snprintf(id, sizeof id, "wave%d", i + 1);
            std::snprintf(nm, sizeof nm, "CHOP %d", i + 1);
            addText(id, nm);
        }
        addText("diag", "DIAGNOSE");
        for (int i = 0; i < kRows; ++i)
        {
            char id[16], nm[16];
            std::snprintf(id, sizeof id, "rowt%d", i + 1);
            std::snprintf(nm, sizeof nm, "ZEILE %d", i + 1);
            addText(id, nm);
        }
        for (int i = 0; i < kRows; ++i)
        {
            char id[16], nm[16];
            std::snprintf(id, sizeof id, "rowb%d", i + 1);
            std::snprintf(nm, sizeof nm, "TIPP %d", i + 1);
            addParam(boolParam(id, nm, false));
        }
        addParam(boolParam("back", "ZURUECK", false));
        addParam(boolParam("drvbtn", "LAUFWERKE BTN", false));
        addParam(boolParam("pgup", "SEITE ZURUECK", false));
        addParam(boolParam("pgdn", "SEITE VOR", false));
        addText("pageinfo", "SEITE");
        for (int i = 0; i < P_COUNT; ++i) texts_.push_back(std::string());
        seenBrowse_ = getInt(P_BROWSE);
        seenOpen_ = getInt(P_OPEN);
        seenSave_ = getInt(P_SAVE);
        prevBrowse_ = seenBrowse_; prevOpen_ = seenOpen_; prevSave_ = seenSave_;
        for (int i = 0; i < kButtons; ++i) btnSeen_[i] = btnPrev_[i] = getInt(P_ROWB0 + i);
        for (auto& r : retired_) r.store(nullptr);
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
        i.version = 4000;
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
            // the built-in loop right away, so the pads sound from the first block on
            src_ = makeBuiltinLoop();
            cutAndPublish();
            worker_ = std::thread([this] { workerMain(); });
        }
    }

    void reset() override
    {
        for (Voice& v : voices_) v.active = false;
        gainSmooth_ = -1.f;
    }

    // ------------------------------------------------------------------ audio
    void process(ProcessContext& ctx) override
    {
        // a new chop set from the worker? (all voices stop: they belong to the old one)
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

        const int padBase = kPadNotes[clamp(getInt(P_PADNOTE), 0, 3)];
        const float pitch = (float)getInt(P_PITCH);
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
                if (e.isNoteOn())
                {
                    lastNote_.store(e.note(), std::memory_order_relaxed);
                    int pad = e.note() - padBase;
                    if (cur_ && pad >= 0 && pad < cur_->info.count) startVoice(pad, e.velocity() / 127.f, pitch);
                }
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
        for (const Voice& v : voices_) if (v.active && v.pad >= 0 && v.pad < 64) mask |= (uint64_t)1 << v.pad;
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
        // the endless controls were set by the project: that is no turn of the user
        seenBrowse_ = getInt(P_BROWSE);
        seenOpen_ = getInt(P_OPEN);
        seenSave_ = getInt(P_SAVE);
        prevBrowse_ = seenBrowse_; prevOpen_ = seenOpen_; prevSave_ = seenSave_;
        for (int i = 0; i < kButtons; ++i) btnSeen_[i] = btnPrev_[i] = getInt(P_ROWB0 + i);
    }

    // ------------------------------------------------------------------ screen texts (UI thread)
    bool formatValue(int index, float plain, std::string& out) override
    {
        (void)plain;
        std::lock_guard<std::mutex> g(mtx_);
        // the endless controls act here too, so the text the MPC reads right after a turn is already the new one
        // BROWSE / OPEN: only a preview here. The MPC sets a value for a moment to ask for its text and sets it back;
        // that must not count as a turn. The worker acts once a new value has stayed (see stableTurn).
        if (index == P_BROWSE)
        {
            if (getInt(P_BROWSE) != seenBrowse_) ++asked_;
            out = browseTextLocked(previewCursorLocked());
            return true;
        }
        if (index == P_OPEN)
        {
            if (getInt(P_OPEN) != seenOpen_) ++asked_;
            int c = previewCursorLocked();
            out = entries_.empty() ? std::string("(leer)") : "oeffnen: " + entries_[(size_t)c].label;
            return true;
        }
        if (index == P_SAVE)   { out = saveRequested_ ? std::string("sichert ...") : std::string("drehen = sichern"); return true; }
        if (index == P_PATH)   { out = cwd_.empty() ? std::string("Laufwerke") : pathLabel(cwd_); return true; }
        if (index >= P_ROWT0 && index < P_ROWT0 + kRows) { out = rowTextLocked(index - P_ROWT0); return true; }
        if (index == P_PAGEINFO)
        {
            char b[48];
            int pages = std::max(1, ((int)entries_.size() + kRows - 1) / kRows);
            std::snprintf(b, sizeof b, "Seite %d/%d, %d Eintraege", page_ + 1, pages, (int)entries_.size());
            out = b;
            return true;
        }
        if (isText(index)) { out = texts_[(size_t)index]; return true; }
        return false;
    }

private:
    // ------------------------------------------------------------------ helpers
    void addText(const char* id, const char* name)
    {
        Param p = floatParam(id, name, 0.f, 1.f, 0.f);
        p.automatable = false;
        textParams_.push_back(addParam(p));
    }
    bool isText(int index) const
    {
        return std::find(textParams_.begin(), textParams_.end(), index) != textParams_.end();
    }

    // "/nvme/Samples" -> "SSD/Samples", "/media/Archive/x" -> "Archive/x"
    static std::string pathLabel(const std::string& p)
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

    void startVoice(int pad, float vel, float pitch)
    {
        // the same pad cuts itself off (fade), like a mono pad on the MPC
        for (Voice& v : voices_) if (v.active && v.pad == pad && v.release < 0) v.release = kFadeOut / 2;
        Voice* slot = nullptr;
        for (Voice& v : voices_) if (!v.active) { slot = &v; break; }
        if (!slot)
        {
            slot = &voices_[0];
            for (Voice& v : voices_) if (v.age > slot->age) slot = &v;
        }
        slot->active = true;
        slot->pad = pad;
        slot->pos = (double)cur_->start[pad];
        slot->end = (double)cur_->end[pad];
        slot->rate = (double)cur_->data.sampleRate / sr_ * std::pow(2.0, pitch / 12.0);
        slot->gain = 0.6f * (0.2f + 0.8f * vel);
        slot->age = 0;
        slot->release = -1;
        lastPad_.store(pad, std::memory_order_relaxed);
        hitCount_.fetch_add(1, std::memory_order_relaxed);
        if (choplink::LinkData* L = link_.data())
        {
            L->lastPad.store(pad, std::memory_order_relaxed);
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
        // all slots full (worker is far behind): leak rather than free on the audio thread
    }

    // ------------------------------------------------------------------ worker thread
    void setText(int index, const std::string& s)
    {
        std::lock_guard<std::mutex> g(mtx_);
        if (texts_[(size_t)index] != s) { texts_[(size_t)index] = s; textsDirty_ = true; }
    }

    // ---- the browser (its state is shared with the UI thread: mtx_ held by the callers of *Locked) ----
    struct Entry { std::string label, path; int kind; }; // kind: 0 up, 1 folder, 2 wav, 3 drive

    // Every mount point that can hold samples: /nvme, /media/*, /mnt/*, /run/media/*/* (from /proc/mounts).
    static std::vector<std::string> findDrives(bool all)
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
                // /proc/mounts writes a space as \040
                std::string m;
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

    // Fills entries_ for cwd_ ("" = the drives, "*" = every mount). Keeps the cursor on `select` if given.
    void listLocked(const std::string& select = std::string())
    {
        entries_.clear();
        if (cwd_.empty() || cwd_ == "*")
        {
            for (const std::string& d : findDrives(cwd_ == "*")) entries_.push_back({pathLabel(d) + "/", d, 3});
            if (cwd_.empty()) entries_.push_back({"(alle Mounts)/", "*", 1});
            else entries_.insert(entries_.begin(), {"..", "", 0});
        }
        else
        {
            entries_.push_back({"..", "", 0});
            for (const std::string& n : listDir(cwd_, true)) entries_.push_back({n + "/", cwd_ + "/" + n, 1});
            for (const std::string& n : listDir(cwd_, false)) entries_.push_back({stripWav(n), cwd_ + "/" + n, 2});
        }
        cursor_ = 0;
        if (!select.empty())
            for (size_t i = 0; i < entries_.size(); ++i) if (entries_[i].path == select) { cursor_ = (int)i; break; }
        if (cursor_ == 0 && entries_.size() > 1 && entries_[0].kind == 0) cursor_ = 1; // start on the first real entry
        page_ = cursor_ / kRows;
        textsDirty_ = true;
        browserDirty_ = true;
    }

    // BROWSE is endless: only the change since the last look counts (one wheel step = one entry).
    static int stepOf(int d) { return d == 0 ? 0 : d > 0 ? 1 + (d - 1) / 6 : -1 - (-d - 1) / 6; } // a wheel click = one entry
    int previewCursorLocked() const
    {
        if (entries_.empty()) return 0;
        return clamp(cursor_ + stepOf(getInt(P_BROWSE) - seenBrowse_), 0, (int)entries_.size() - 1);
    }
    void stepBrowserLocked(int v)
    {
        int d = v - seenBrowse_;
        seenBrowse_ = v;
        if (d == 0 || entries_.empty()) return;
        cursor_ = clamp(cursor_ + stepOf(d), 0, (int)entries_.size() - 1);
        textsDirty_ = true;
    }

    std::string browseTextLocked(int cur) const
    {
        if (entries_.empty()) return "(leer)";
        char buf[16];
        std::snprintf(buf, sizeof buf, "  %d/%d", cur + 1, (int)entries_.size());
        std::string label = entries_[(size_t)cur].label;
        if (label.size() > 22) label = label.substr(0, 21) + "~";
        return label + buf;
    }

    std::string parentOf(const std::string& p) const
    {
        for (const std::string& d : findDrives(false)) if (p == d) return std::string();
        size_t i = p.find_last_of('/');
        return i == std::string::npos || i == 0 ? std::string() : p.substr(0, i);
    }

    // ---- the BROWSER page ----
    std::string rowTextLocked(int row) const
    {
        size_t i = (size_t)(page_ * kRows + row);
        if (i >= entries_.size()) return std::string();
        const Entry& e = entries_[i];
        if (e.kind == 0) return "..  (zurueck)";
        if (e.kind == 1 || e.kind == 3) return "[" + e.label.substr(0, e.label.size() - 1) + "]";
        std::string mark = (!loadedPath_.empty() && e.path == loadedPath_) ? "   < geladen" : "";
        return e.label + ".wav" + mark;
    }

    void goUpLocked()
    {
        if (cwd_.empty()) return;
        std::string from = cwd_;
        cwd_ = cwd_ == "*" ? std::string() : parentOf(cwd_);
        listLocked(from);
        if (cwd_ != "*") { lastDir_ = cwd_; lastDirDirty_ = true; }
    }

    void tapRowLocked(int row)
    {
        int i = page_ * kRows + row;
        if (i < 0 || i >= (int)entries_.size()) return;
        cursor_ = i;
        const Entry e = entries_[(size_t)i];
        ++opened_;
        if (e.kind == 0) goUpLocked();
        else if (e.kind == 1 || e.kind == 3)
        {
            cwd_ = e.path;
            listLocked();
            if (cwd_ != "*") { lastDir_ = cwd_; lastDirDirty_ = true; }
        }
        else { requestedPath_ = e.path; pathRequest_ = true; }
        browserDirty_ = true;
    }

    // OPEN is endless too: any turn opens what BROWSE shows.
    void openLocked(int v)
    {
        if (v == seenOpen_) return;
        seenOpen_ = v;
        ++opened_;
        if (entries_.empty()) return;
        const Entry e = entries_[(size_t)cursor_];
        if (e.kind == 0) { std::string from = cwd_; cwd_ = cwd_ == "*" ? std::string() : parentOf(cwd_); listLocked(from); }
        else if (e.kind == 1 || e.kind == 3) { cwd_ = e.path; listLocked(); }
        else { requestedPath_ = e.path; pathRequest_ = true; }
        if (cwd_ != "*") { lastDir_ = cwd_; lastDirDirty_ = true; }
    }

    // LOAD's switch shows On/Off (the MPC needs a real on/off there); what loading does is shown in ANALYSE
    void setLoadText(const std::string& s) { setText(P_ANALYSE, s); }

    // Reads a WAV into src_. Returns false (and shows the error) if it cannot.
    bool loadPath(const std::string& path)
    {
        setLoadText("laedt ...");
        std::vector<uint8_t> bytes;
        if (!mpc::readFile(path, bytes)) { setLoadText("Fehler: nicht lesbar"); return false; }
        hh::SampleData d;
        std::string err;
        if (!hh::readWav(bytes, d, err, 60.f)) { setLoadText("Fehler: " + err); return false; }
        d.path = path;
        d.name = stripWav(baseName(path));
        src_ = std::move(d);
        {
            std::lock_guard<std::mutex> g(mtx_);
            loadedPath_ = path;
        }
        return true;
    }

    // Cuts src_ with the current MODE / CHOPS, publishes it to the audio thread and updates the texts.
    void cutAndPublish()
    {
        if (src_.frames() == 0) return;
        auto t0 = std::chrono::steady_clock::now();
        hh::ChopSet* cs = new hh::ChopSet();
        cs->data = src_;
        int mode = getInt(P_MODE) == 1 ? hh::CM_EQUAL : hh::CM_TRANSIENT;
        int count = clamp(getInt(P_CHOPS), 4, 64);
        hh::analyse(*cs, mode, count);
        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        cutMode_ = getInt(P_MODE);
        cutCount_ = getInt(P_CHOPS);

        // what the screen and CHOP LINK need, copied before the set goes to the audio thread
        info_ = cs->info;
        sampleBpm_ = cs->bpm;
        for (int p = 0; p < 64; ++p)
        {
            float pk = 0.f;
            if (p < cs->info.count)
                for (size_t i = cs->start[p]; i < cs->end[p] && i < cs->data.L.size(); ++i)
                    pk = std::max(pk, std::fabs(cs->data.L[i]));
            chopPeak_[p] = pk;
            chopStart_[p] = p < cs->info.count ? cs->start[p] : 0;
            chopEnd_[p] = p < cs->info.count ? cs->end[p] : 0;
        }
        float top = 1e-6f;
        for (int p = 0; p < 64; ++p) top = std::max(top, chopPeak_[p]);
        for (int p = 0; p < 64; ++p) chopPeak_[p] /= top;

        // hand it over: free what the audio thread gave back, then publish
        for (auto& r : retired_) delete r.exchange(nullptr);
        delete pending_.exchange(cs, std::memory_order_acq_rel);

        char buf[64];
        std::snprintf(buf, sizeof buf, "OK, %d Chops, %.2f s", info_.count, secs);
        setText(P_ANALYSE, buf);
        char k[16];
        hh::keyName(info_.key, k, sizeof k);
        std::snprintf(buf, sizeof buf, "%s (%d%%)", k, (int)std::lround(info_.keyConf * 100.f));
        setText(P_KEY, buf);
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
        bankShown_ = -1; // redraw chords / bars
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

    std::string settingsPath() const
    {
        std::string f = mpc::pluginFolder();
        return f.empty() ? std::string() : f + "/settings.txt";
    }

    // settings.txt remembers the last folder of the browser, for every project
    void saveSettings(const std::string& dir)
    {
        std::string p = settingsPath();
        if (p.empty()) return;
        std::string s = "lastdir=" + dir + "\n";
        setText(P_WRITE, writeFileBytes(p, s.data(), s.size()) ? "OK, Ordner gemerkt" : "Fehler: settings.txt");
    }

    std::string loadSettingsDir() const
    {
        std::string t;
        if (!mpc::readTextFile(settingsPath(), t)) return std::string();
        size_t i = t.find("lastdir=");
        if (i == std::string::npos) return std::string();
        size_t e = t.find('\n', i);
        return t.substr(i + 8, e == std::string::npos ? std::string::npos : e - i - 8);
    }

    void saveChop()
    {
        int pad = lastPad_.load();
        if (pad < 0 || pad >= info_.count || src_.frames() == 0) { setSaveText("erst ein Pad spielen"); return; }
        std::string dir;
        {
            std::lock_guard<std::mutex> g(mtx_);
            size_t i = loadedPath_.find_last_of('/');
            dir = i == std::string::npos ? mpc::pluginFolder() : loadedPath_.substr(0, i);
        }
        dir += "/CHOP TEST";
        ::mkdir(dir.c_str(), 0777);
        if (chopEnd_[pad] <= chopStart_[pad]) { setSaveText("Fehler: Pad leer"); return; }
        std::vector<uint8_t> wav = hh::writeWav(src_, chopStart_[pad], chopEnd_[pad], false, getInt(P_PITCH));
        char name[96];
        std::snprintf(name, sizeof name, "/%s pad%02d.wav", sampleName_.substr(0, 60).c_str(), pad + 1);
        bool ok = writeFileBytes(dir + name, wav.data(), wav.size());
        setSaveText(ok ? std::string("OK, WAV Pad ") + std::to_string(pad + 1) : std::string("Fehler: WAV"));
    }

    // likewise SAVE CHOP: the result goes to SCHREIBEN
    void setSaveText(const std::string& s) { setText(P_WRITE, s); }

    void updateScreen()
    {
        char buf[64];
        int pad = lastPad_.load();
        int note = lastNote_.load();
        uint64_t playing = playingMask_.load();

        // PAD: last pad, its chord, the note that came in
        if (note < 0) setText(P_PAD, "noch nichts gespielt");
        else if (pad < 0) { std::snprintf(buf, sizeof buf, "Note %d: kein Pad", note); setText(P_PAD, buf); }
        else
        {
            char c[16];
            hh::Chord ch = hh::padChord(info_, nullptr, pad);
            if (ch.valid()) hh::chordName(ch, c, sizeof c); else std::snprintf(c, sizeof c, "HIT");
            std::snprintf(buf, sizeof buf, "%d: %s (Note %d)", pad + 1, c, note);
            setText(P_PAD, buf);
        }

        // TEMPO: the sample's and the MPC's
        int host = hostBpmX100_.load();
        std::snprintf(buf, sizeof buf, "Sample %.0f / MPC %.0f", (double)sampleBpm_, host / 100.0);
        setText(P_TEMPO, buf);

        // LINK: is a CHOP LINK reading?
        choplink::LinkData* L = link_.data();
        if (!L) setText(P_LINK, "Fehler: " + link_.error());
        else
        {
            uint32_t rb = L->readerBeat.load();
            auto now = std::chrono::steady_clock::now();
            if (rb != lastReaderBeat_) { lastReaderBeat_ = rb; readerSeen_ = now; readerEver_ = true; }
            bool alive = readerEver_ && now - readerSeen_ < std::chrono::milliseconds(1500);
            setText(P_LINK, alive ? "CHOP LINK hoert zu" : "kein CHOP LINK");
        }

        // the current bank (of the last pad): chords and bars
        int bank = pad < 0 ? 0 : pad / 16;
        bool notify = getInt(P_UPDATE) == 1;
        for (int k = 0; k < 16; ++k)
        {
            int p = bank * 16 + k;
            char c[16];
            if (p >= info_.count) std::snprintf(c, sizeof c, "-");
            else
            {
                hh::Chord ch = hh::padChord(info_, nullptr, p);
                if (ch.valid()) hh::chordName(ch, c, sizeof c); else std::snprintf(c, sizeof c, "HIT");
            }
            std::snprintf(buf, sizeof buf, "%c%d  %s", "ABCD"[bank], k + 1, c);
            setText(P_CHORD0 + k, buf);

            bool on = (playing >> p) & 1u;
            float v = p < info_.count ? chopPeak_[p] : 0.f;
            if (p < info_.count)
            {
                // the level as text too: 10 characters, # = level, playing chops get "> "
                char bar[12];
                int n = clamp((int)std::lround(v * 10.f), 0, 10);
                for (int j = 0; j < 10; ++j) bar[j] = j < n ? '#' : '-';
                bar[10] = 0;
                std::snprintf(buf, sizeof buf, "%s%d %s", on ? "> " : "", p + 1, bar);
            }
            else
                std::snprintf(buf, sizeof buf, "%d leer", p + 1);
            setText(P_WAVE0 + k, buf);
            if (std::fabs(getNormalised(P_WAVE0 + k) - v) > 1e-4f)
            {
                setNormalised(P_WAVE0 + k, v);
                if (notify) notifyHostParameterChanged(P_WAVE0 + k);
            }
        }
        bankShown_ = bank;

        bool dirty;
        {
            std::lock_guard<std::mutex> g(mtx_);
            dirty = textsDirty_;
            textsDirty_ = false;
        }
        if (dirty && notify) requestDisplayUpdate();
    }

    // The MPC re-reads a text only when it thinks the value changed: nudge the browser's text parameters,
    // tell the host (audioMasterAutomate) and ask for a redraw (audioMasterUpdateDisplay).
    void refreshBrowser()
    {
        bool dirty;
        {
            std::lock_guard<std::mutex> g(mtx_);
            dirty = browserDirty_;
            browserDirty_ = false;
        }
        if (!dirty) return;
        refreshTick_ = !refreshTick_;
        const float v = refreshTick_ ? 1.f : 0.f;
        int list[kRows + 3];
        int n = 0;
        for (int i = 0; i < kRows; ++i) list[n++] = P_ROWT0 + i;
        list[n++] = P_PATH; list[n++] = P_PAGEINFO; list[n++] = P_LOADED;
        for (int i = 0; i < n; ++i)
        {
            setNormalised(list[i], v);
            notifyHostParameterChanged(list[i]);
        }
        requestDisplayUpdate();
    }

    void workerMain()
    {
        using clock = std::chrono::steady_clock;
        setText(P_LINK, linkOk_ ? "bereit (" + link_.path() + ")" : "Fehler: " + link_.error());
        writeTest();
        {
            std::lock_guard<std::mutex> g(mtx_);
            std::string last = loadSettingsDir();
            cwd_ = (!last.empty() && isDir(last)) ? last : std::string();
            listLocked();
        }
        clock::time_point cutChanged{}, lastDrives{};
        int lastMode = getInt(P_MODE), lastChops = getInt(P_CHOPS);
        bool cutPending = false;

        while (!quit_.load())
        {
            auto now = clock::now();

            // drives (shown in LAUFWERKE; the drive list of the browser follows when it is shown)
            if (now - lastDrives > std::chrono::seconds(3))
            {
                lastDrives = now;
                std::string names;
                for (const std::string& d : findDrives(false)) names += (names.empty() ? "" : ", ") + pathLabel(d);
                setText(P_DRIVES, names.empty() ? std::string("keins gefunden") : names);
                std::lock_guard<std::mutex> g(mtx_);
                if (cwd_.empty()) { std::string sel = entries_.empty() ? std::string() : entries_[(size_t)cursor_].path; listLocked(sel); }
            }

            // the endless controls: a new value counts once it has stayed for two looks (>= 40 ms);
            // the MPC's quick "what would the text be" probes are set back long before that
            std::string want, dirToSave;
            {
                std::lock_guard<std::mutex> g(mtx_);
                int b = getInt(P_BROWSE), o = getInt(P_OPEN), sv = getInt(P_SAVE);
                if (b != seenBrowse_ && b == prevBrowse_) stepBrowserLocked(b);
                if (o != seenOpen_ && o == prevOpen_) openLocked(o);
                if (sv != seenSave_ && sv == prevSave_) { seenSave_ = sv; saveRequested_ = true; }
                prevBrowse_ = b; prevOpen_ = o; prevSave_ = sv;
                // the BROWSER page's buttons: a change that has stayed for two looks is a tap
                for (int k = 0; k < kButtons; ++k)
                {
                    int v = getInt(P_ROWB0 + k);
                    if (v != btnSeen_[k] && v == btnPrev_[k])
                    {
                        btnSeen_[k] = v;
                        if (k < kRows) tapRowLocked(k);
                        else if (k == kRows) { goUpLocked(); browserDirty_ = true; }
                        else if (k == kRows + 1) { cwd_.clear(); listLocked(); browserDirty_ = true; }
                        else if (k == kRows + 2) { page_ = std::max(0, page_ - 1); browserDirty_ = true; }
                        else
                        {
                            int pages = std::max(1, ((int)entries_.size() + kRows - 1) / kRows);
                            page_ = std::min(pages - 1, page_ + 1);
                            browserDirty_ = true;
                        }
                    }
                    btnPrev_[k] = v;
                }
                if (pathRequest_) { want = requestedPath_; pathRequest_ = false; }
                if (lastDirDirty_) { dirToSave = lastDir_; lastDirDirty_ = false; }
            }
            if (!dirToSave.empty()) saveSettings(dirToSave);
            if (!want.empty())
            {
                if (loadPath(want)) cutAndPublish();
                std::lock_guard<std::mutex> g(mtx_);
                browserDirty_ = true;
            }
            refreshBrowser();

            // MODE / CHOPS: cut again once the knob rests
            int m = getInt(P_MODE), c = getInt(P_CHOPS);
            if (m != lastMode || c != lastChops) { lastMode = m; lastChops = c; cutPending = true; cutChanged = now; }
            if (cutPending && now - cutChanged > std::chrono::milliseconds(300)) { cutPending = false; cutAndPublish(); }

            // SAVE CHOP
            bool save;
            {
                std::lock_guard<std::mutex> g(mtx_);
                save = saveRequested_;
            }
            if (save)
            {
                saveChop();
                std::lock_guard<std::mutex> g(mtx_);
                saveRequested_ = false;
                textsDirty_ = true;
            }

            {
                char d[48];
                int asked, opened;
                {
                    std::lock_guard<std::mutex> g(mtx_);
                    asked = asked_; opened = opened_;
                }
                std::snprintf(d, sizeof d, "Abfragen %d, geoeffnet %d", asked, opened);
                setText(P_DIAG, d);
            }
            setText(P_LOADED, sampleName_.empty() ? std::string("Testloop (eingebaut)") : sampleName_);
            updateScreen();
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
        }
    }

    // ------------------------------------------------------------------ data
    // audio thread
    hh::ChopSet* cur_ = nullptr;
    Voice voices_[kVoices];
    double sr_ = 44100.0;
    float gainSmooth_ = -1.f;

    // hand-over between worker and audio thread (lock-free)
    std::atomic<hh::ChopSet*> pending_{nullptr};
    std::atomic<hh::ChopSet*> retired_[4];
    std::atomic<int> lastPad_{-1}, lastNote_{-1}, hostBpmX100_{0};
    std::atomic<uint32_t> hitCount_{0};
    std::atomic<uint64_t> playingMask_{0};

    // worker
    std::thread worker_;
    std::atomic<bool> quit_{false};
    bool started_ = false;
    choplink::Link link_;
    bool linkOk_ = false;
    hh::SampleData src_;
    hh::SampleInfo info_;
    float sampleBpm_ = 0.f;
    float chopPeak_[64] = {};
    size_t chopStart_[64] = {}, chopEnd_[64] = {};
    std::string sampleName_;
    int cutMode_ = 0, cutCount_ = 16, bankShown_ = -1;
    uint32_t lastReaderBeat_ = 0;
    std::chrono::steady_clock::time_point readerSeen_{};
    bool readerEver_ = false;

    // shared between worker and UI thread (mtx_)
    std::mutex mtx_;
    std::vector<int> textParams_;
    std::vector<std::string> texts_;
    std::vector<Entry> entries_;
    std::string cwd_, lastDir_;          // "" = the drive list, "*" = every mount
    int cursor_ = 0, seenBrowse_ = 0, seenOpen_ = 0, seenSave_ = 0;
    int prevBrowse_ = 0, prevOpen_ = 0, prevSave_ = 0, asked_ = 0, opened_ = 0;
    static const int kButtons = kRows + 4; // rows, ZURUECK, LAUFWERKE, < SEITE, SEITE >
    int btnSeen_[kButtons] = {}, btnPrev_[kButtons] = {};
    int page_ = 0;
    bool browserDirty_ = true, refreshTick_ = false;
    std::string loadedPath_, requestedPath_;
    bool pathRequest_ = false, textsDirty_ = false, lastDirDirty_ = false, saveRequested_ = false;
};

MPC_PLUGIN_ENTRY(ChopTest)
