// CHOP TEST - the mini test for GENRA CHOP on the MPC.
//
// SPDX-License-Identifier: GPL-3.0-only
//
// What it checks on the device:
//   * the drives (SSD /nvme, SD card, USB under /media) can be listed and WAVs read from them
//   * the GENRA CHOP engine (src/Chop.cpp, Core.cpp, Styles.cpp: copied from Camillowup/genra-chop) cuts and
//     analyses a sample on the MPC's CPU (key, tempo, a chord per chop) and how long that takes
//   * the plugin may write files (settings.txt in its own folder, a chop as WAV into the root folder)
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
    P_ROOT,       // root folder (index into the list of drives and their top folders)
    P_FOLDER,     // folder under the root (index)
    P_SAMPLE,     // WAV file in the folder (index)
    P_LOAD,       // toggle: every change loads the selected sample
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
    P_SAVE,       // toggle: every change saves the last played chop as WAV
    P_UPDATE,     // how the screen learns about new values: Text (polling) / Notify (host notified)
    P_PADNOTE,    // MIDI note of pad 1
    P_CHORD0,     // 16 read-only: chords of the current bank
    P_WAVE0 = P_CHORD0 + 16, // 16 read-only bars: level of each chop of the current bank
    P_COUNT = P_WAVE0 + 16
};

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
        if (n.empty() || n[0] == '.') continue;
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
        addParam(intParam("root", "ROOT", 0, 999, 0));
        addParam(intParam("folder", "FOLDER", 0, 999, 0));
        addParam(intParam("sample", "SAMPLE", 0, 999, 0));
        addParam(boolParam("load", "LOAD", false));
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
        addParam(boolParam("save", "SAVE CHOP", false));
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
        for (int i = 0; i < P_COUNT; ++i) texts_.push_back(std::string());
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
        i.version = 1000;
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
        out.assign(loadedPath_.begin(), loadedPath_.end());
    }

    void loadExtraState(const uint8_t* data, size_t size) override
    {
        std::lock_guard<std::mutex> g(mtx_);
        requestedPath_.assign(reinterpret_cast<const char*>(data), size);
        pathRequest_ = true;
    }

    // ------------------------------------------------------------------ screen texts (UI thread)
    bool formatValue(int index, float plain, std::string& out) override
    {
        std::lock_guard<std::mutex> g(mtx_);
        int i = (int)std::lround(plain);
        switch (index)
        {
            case P_ROOT:   out = roots_.empty() ? std::string("kein Laufwerk") : rootLabel(roots_[(size_t)clamp(i, 0, (int)roots_.size() - 1)]); return true;
            case P_FOLDER: out = folders_.empty() ? std::string("-") : folderLabel(folders_[(size_t)clamp(i, 0, (int)folders_.size() - 1)]); return true;
            case P_SAMPLE: out = samples_.empty() ? std::string("keine WAVs") : stripWav(samples_[(size_t)clamp(i, 0, (int)samples_.size() - 1)]); return true;
            default: break;
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

    static std::string rootLabel(const std::string& p)
    {
        std::string s = p;
        if (s.compare(0, 5, "/nvme") == 0) s = "SSD" + s.substr(5);
        else if (s.compare(0, 7, "/media/") == 0) s = s.substr(7);
        return s.empty() ? p : s;
    }
    std::string folderLabel(const std::string& rel) const
    {
        return rel.empty() ? std::string("(Hauptordner)") : rel;
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

    void scanDrives()
    {
        std::vector<std::string> drives;
        if (const char* env = std::getenv("CHOP_TEST_DRIVE")) if (isDir(env)) drives.push_back(env); // for tests on a computer
        if (isDir("/nvme")) drives.push_back("/nvme");
        for (const std::string& m : listDir("/media", true)) drives.push_back("/media/" + m);
        std::vector<std::string> roots;
        std::string names;
        for (const std::string& d : drives)
        {
            roots.push_back(d);
            for (const std::string& sub : listDir(d, true)) roots.push_back(d + "/" + sub);
            if (!names.empty()) names += ", ";
            names += rootLabel(d);
        }
        {
            std::lock_guard<std::mutex> g(mtx_);
            if (roots != roots_) { roots_ = roots; textsDirty_ = true; listsChanged_ = true; }
        }
        setText(P_DRIVES, drives.empty() ? std::string("keins gefunden") : names);
    }

    std::string currentRoot()
    {
        std::lock_guard<std::mutex> g(mtx_);
        if (roots_.empty()) return std::string();
        return roots_[(size_t)clamp(getInt(P_ROOT), 0, (int)roots_.size() - 1)];
    }

    void scanFolders(const std::string& root)
    {
        std::vector<std::string> out;
        out.push_back(std::string()); // the root itself
        // breadth first, 3 levels, at most 400 folders
        std::vector<std::string> level{std::string()};
        for (int depth = 0; depth < 3 && out.size() < 400; ++depth)
        {
            std::vector<std::string> next;
            for (const std::string& rel : level)
                for (const std::string& sub : listDir(rel.empty() ? root : root + "/" + rel, true))
                {
                    std::string r = rel.empty() ? sub : rel + "/" + sub;
                    next.push_back(r);
                    out.push_back(r);
                    if (out.size() >= 400) break;
                }
            level.swap(next);
        }
        std::sort(out.begin() + 1, out.end(), lessNoCase);
        std::lock_guard<std::mutex> g(mtx_);
        folders_ = out;
        textsDirty_ = true;
    }

    std::string currentFolderPath(const std::string& root)
    {
        std::lock_guard<std::mutex> g(mtx_);
        if (folders_.empty()) return root;
        const std::string& rel = folders_[(size_t)clamp(getInt(P_FOLDER), 0, (int)folders_.size() - 1)];
        return rel.empty() ? root : root + "/" + rel;
    }

    void scanSamples(const std::string& folder)
    {
        std::vector<std::string> s = listDir(folder, false);
        std::lock_guard<std::mutex> g(mtx_);
        samples_ = s;
        sampleFolder_ = folder;
        textsDirty_ = true;
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

    void saveSettings(const std::string& root)
    {
        std::string p = settingsPath();
        if (p.empty() || root.empty()) return;
        std::string s = "root=" + root + "\n";
        setText(P_WRITE, writeFileBytes(p, s.data(), s.size()) ? "OK, ROOT gespeichert" : "Fehler: settings.txt");
    }

    std::string loadSettingsRoot() const
    {
        std::string t;
        if (!mpc::readTextFile(settingsPath(), t)) return std::string();
        size_t i = t.find("root=");
        if (i == std::string::npos) return std::string();
        size_t e = t.find('\n', i);
        return t.substr(i + 5, e == std::string::npos ? std::string::npos : e - i - 5);
    }

    void saveChop()
    {
        int pad = lastPad_.load();
        if (pad < 0 || pad >= info_.count || src_.frames() == 0) { setSaveText("erst ein Pad spielen"); return; }
        std::string root = currentRoot();
        std::string dir = root.empty() ? mpc::pluginFolder() : root;
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
            std::snprintf(buf, sizeof buf, "%s%d%s", on ? "> " : "", p + 1, p < info_.count ? "" : " leer");
            setText(P_WAVE0 + k, buf);
            float v = p < info_.count ? chopPeak_[p] : 0.f;
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

    void workerMain()
    {
        using clock = std::chrono::steady_clock;
        setText(P_LINK, linkOk_ ? "bereit (" + link_.path() + ")" : "Fehler: " + link_.error());
        writeTest();
        scanDrives();

        // the root from settings.txt (set once, for every project)
        std::string savedRoot = loadSettingsRoot();
        if (!savedRoot.empty())
        {
            std::lock_guard<std::mutex> g(mtx_);
            for (size_t i = 0; i < roots_.size(); ++i)
                if (roots_[i] == savedRoot) { set(P_ROOT, (float)i); break; }
        }

        int lastRoot = -1, lastFolder = -1;
        float lastLoad = getNormalised(P_LOAD), lastSave = getNormalised(P_SAVE);
        int lastMode = getInt(P_MODE), lastChops = getInt(P_CHOPS);
        clock::time_point cutChanged{}, rootChanged{}, lastDrives = clock::now();
        bool cutPending = false, rootDirty = false;
        bool haveSample = true; // the built-in loop from prepare()

        while (!quit_.load())
        {
            auto now = clock::now();

            // a project brought its sample along
            std::string want;
            {
                std::lock_guard<std::mutex> g(mtx_);
                if (pathRequest_) { want = requestedPath_; pathRequest_ = false; }
            }
            if (!want.empty() && loadPath(want)) { cutAndPublish(); haveSample = true; }

            // nothing loaded yet: the built-in loop, so the pads always sound
            if (!haveSample)
            {
                src_ = makeBuiltinLoop();
                cutAndPublish();
                haveSample = true;
                {
                    std::lock_guard<std::mutex> g(mtx_);
                    loadedPath_.clear();
                }
            }

            // drives come and go (USB stick, SD card)
            if (now - lastDrives > std::chrono::seconds(5)) { scanDrives(); lastDrives = now; }

            // ROOT / FOLDER -> lists
            bool lists;
            {
                std::lock_guard<std::mutex> g(mtx_);
                lists = listsChanged_;
                listsChanged_ = false;
            }
            int r = getInt(P_ROOT), f = getInt(P_FOLDER);
            if (r != lastRoot || lists)
            {
                if (lastRoot >= 0 && r != lastRoot) { rootDirty = true; rootChanged = now; }
                lastRoot = r;
                scanFolders(currentRoot());
                lastFolder = -1;
            }
            if (f != lastFolder)
            {
                lastFolder = f;
                scanSamples(currentFolderPath(currentRoot()));
            }
            if (rootDirty && now - rootChanged > std::chrono::milliseconds(1000)) { saveSettings(currentRoot()); rootDirty = false; }

            // LOAD (every change of the toggle)
            float ld = getNormalised(P_LOAD);
            if (ld != lastLoad)
            {
                lastLoad = ld;
                std::string path;
                {
                    std::lock_guard<std::mutex> g(mtx_);
                    if (!samples_.empty()) path = sampleFolder_ + "/" + samples_[(size_t)clamp(getInt(P_SAMPLE), 0, (int)samples_.size() - 1)];
                }
                if (path.empty()) setLoadText("kein Sample gewaehlt");
                else if (loadPath(path)) cutAndPublish();
            }

            // MODE / CHOPS: cut again once the knob rests
            int m = getInt(P_MODE), c = getInt(P_CHOPS);
            if (m != lastMode || c != lastChops) { lastMode = m; lastChops = c; cutPending = true; cutChanged = now; }
            if (cutPending && now - cutChanged > std::chrono::milliseconds(300)) { cutPending = false; cutAndPublish(); }

            // SAVE CHOP
            float sv = getNormalised(P_SAVE);
            if (sv != lastSave) { lastSave = sv; saveChop(); }

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
    std::vector<std::string> roots_, folders_, samples_;
    std::string sampleFolder_, loadedPath_, requestedPath_;
    bool pathRequest_ = false, textsDirty_ = false, listsChanged_ = false;
};

MPC_PLUGIN_ENTRY(ChopTest)
