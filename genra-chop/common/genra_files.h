// genra_files.h - files for the GENRA plugins on the MPC: drives, the tap browser, the Genra folder with the
// settings and the shared pad map, WAV reading (for SCHLAG and the previews), MIDI files for PRINT.
//
// SPDX-License-Identifier: GPL-3.0-only
//
// The Genra folder: <drive>/Genra, next to "VST2 Plugins" (so updating a plugin folder keeps everything):
//   Genra/pads.txt        the learned pad notes, shared by GENRA CHOP and SCHLAG (learn once)
//   Genra/<PLUGIN>.txt    each plugin's own settings (last folder, save folder, ...)
//   Genra/PRINT/          the prints (MIDI files), unless a plugin was told another folder
// Fallback when the plugin does not sit in a "VST2 Plugins" folder: its own folder.
// Everything here runs on worker threads, never in process().

#ifndef GENRA_FILES_H
#define GENRA_FILES_H

#include "mpc_plugin.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace genrafiles {

inline std::string baseName(const std::string& p)
{
    size_t i = p.find_last_of('/');
    return i == std::string::npos ? p : p.substr(i + 1);
}

inline std::string stripWav(const std::string& n)
{
    if (n.size() > 4)
    {
        std::string e = n.substr(n.size() - 4);
        std::transform(e.begin(), e.end(), e.begin(), ::tolower);
        if (e == ".wav") return n.substr(0, n.size() - 4);
    }
    return n;
}

inline bool hasExt(const std::string& n, const char* ext)
{
    size_t k = std::strlen(ext);
    if (n.size() <= k) return false;
    std::string e = n.substr(n.size() - k);
    std::transform(e.begin(), e.end(), e.begin(), ::tolower);
    return e == ext;
}

inline bool isDir(const std::string& p)
{
    struct stat st;
    return ::stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

inline bool lessNoCase(const std::string& a, const std::string& b)
{
    size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i)
    {
        int x = ::tolower((unsigned char)a[i]), y = ::tolower((unsigned char)b[i]);
        if (x != y) return x < y;
    }
    return a.size() < b.size();
}

// Entries of a folder: sub folders, or files with the extension (no hidden entries, no system folders).
inline std::vector<std::string> listDir(const std::string& dir, bool wantDirs, const char* ext = ".wav")
{
    std::vector<std::string> out;
    DIR* d = ::opendir(dir.c_str());
    if (!d) return out;
    while (dirent* e = ::readdir(d))
    {
        std::string n = e->d_name;
        if (n.empty() || n[0] == '.' || n[0] == '$') continue;
        bool dirp = isDir(dir + "/" + n);
        if (wantDirs && dirp)
        {
            if (n == "lost+found" || n == "System Volume Information" || n == "VST2 Plugins") continue;
            out.push_back(n);
        }
        else if (!wantDirs && !dirp && hasExt(n, ext))
            out.push_back(n);
        if (out.size() >= 2000) break;
    }
    ::closedir(d);
    std::sort(out.begin(), out.end(), lessNoCase);
    return out;
}

inline bool writeFileBytes(const std::string& path, const void* data, size_t n)
{
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = std::fwrite(data, 1, n, f) == n;
    ok = (std::fclose(f) == 0) && ok;
    return ok;
}

// "/nvme/Samples" -> "SSD/Samples", "/media/Archive/x" -> "Archive/x"
inline std::string pathLabel(const std::string& p)
{
    if (p == "*") return "(all mounts)";
    if (p.compare(0, 5, "/nvme") == 0) return "SSD" + p.substr(5);
    for (const char* pre : {"/media/", "/mnt/", "/run/media/"})
    {
        size_t n = std::strlen(pre);
        if (p.compare(0, n, pre) == 0) return p.substr(n);
    }
    return p;
}

// Every mount point that can hold samples (from /proc/mounts); all = every mount except /, /proc, /sys, /dev.
inline std::vector<std::string> findDrives(bool all)
{
    std::vector<std::string> out;
    if (const char* env = std::getenv("GENRA_DRIVE")) if (isDir(env)) out.push_back(env); // tests on a computer
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

// ---------------------------------------------------------------- the Genra folder and settings
inline std::string genraDir()
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
        if (isDir(dir)) return dir;
    }
    return f;
}

// key=value lines in a text file
class Settings
{
public:
    explicit Settings(const std::string& file) : file_(file) {}
    std::string path() const { std::string d = genraDir(); return d.empty() ? std::string() : d + "/" + file_; }
    void load()
    {
        text_.clear();
        mpc::readTextFile(path(), text_);
    }
    std::string get(const std::string& key) const
    {
        size_t i = 0;
        while ((i = text_.find(key + "=", i)) != std::string::npos)
        {
            if (i == 0 || text_[i - 1] == '\n')
            {
                size_t e = text_.find('\n', i);
                size_t a = i + key.size() + 1;
                return text_.substr(a, e == std::string::npos ? std::string::npos : e - a);
            }
            i += key.size();
        }
        return std::string();
    }
    void set(const std::string& key, const std::string& value)
    {
        std::string out;
        size_t pos = 0;
        bool done = false;
        while (pos < text_.size())
        {
            size_t e = text_.find('\n', pos);
            std::string line = text_.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
            pos = e == std::string::npos ? text_.size() : e + 1;
            if (line.empty()) continue;
            if (line.compare(0, key.size() + 1, key + "=") == 0) { line = key + "=" + value; done = true; }
            out += line + "\n";
        }
        if (!done) out += key + "=" + value + "\n";
        text_ = out;
    }
    bool save() const
    {
        std::string p = path();
        return !p.empty() && writeFileBytes(p, text_.data(), text_.size());
    }

private:
    std::string file_, text_;
};

// ---------------------------------------------------------------- the pad map (learned once, used by every plugin)
// note -> pad (0..15), -1: none. Without a learned map: pad k = note 36 + k.
class PadMap
{
public:
    PadMap() { for (auto& n : noteToPad_) n.store(-1); }
    bool learned() const { return learned_.load(); }
    int padOf(int note) const
    {
        if (note < 0 || note > 127) return -1;
        if (learned_.load(std::memory_order_relaxed)) return noteToPad_[note].load(std::memory_order_relaxed);
        return note >= 36 && note < 52 ? note - 36 : -1;
    }
    int noteOf(int pad) const
    {
        if (learned_.load())
            for (int n = 0; n < 128; ++n) if (noteToPad_[n].load() == pad) return n;
        return 36 + pad;
    }
    // reads Genra/pads.txt (and, once, an older plugin's own file)
    void load()
    {
        Settings s("pads.txt");
        s.load();
        std::string m = s.get("padmap");
        if (m.empty()) { Settings old("CHOP TEST.txt"); old.load(); m = old.get("padmap"); }
        fromString(m);
    }
    bool save() const
    {
        Settings s("pads.txt");
        s.load();
        s.set("padmap", toString());
        return s.save();
    }
    void setLearned(const int* notes16)
    {
        for (auto& n : noteToPad_) n.store(-1);
        for (int k = 0; k < 16; ++k) if (notes16[k] >= 0 && notes16[k] < 128) noteToPad_[notes16[k]].store((int8_t)k);
        learned_.store(true);
    }
    std::string toString() const
    {
        if (!learned_.load()) return std::string();
        std::string s;
        for (int k = 0; k < 16; ++k) s += (k ? "," : "") + std::to_string(noteOf(k));
        return s;
    }
    void fromString(const std::string& m)
    {
        std::vector<int> notes;
        size_t pos = 0;
        while (pos < m.size())
        {
            size_t e = m.find(',', pos);
            notes.push_back(std::atoi(m.substr(pos, e == std::string::npos ? std::string::npos : e - pos).c_str()));
            if (e == std::string::npos) break;
            pos = e + 1;
        }
        if ((int)notes.size() != 16) return;
        setLearned(notes.data());
    }
    // learning: the next 16 different notes are pads 1..16
    void startLearning() { for (auto& n : learnNotes_) n.store(-1); learnStep_.store(0); learning_.store(true); }
    bool learning() const { return learning_.load(std::memory_order_acquire); }
    int learnStep() const { return learnStep_.load(); }
    // audio thread: true if the note was taken by learning (then it must not play)
    bool learnNote(int note)
    {
        if (!learning_.load(std::memory_order_acquire)) return false;
        int step = learnStep_.load(std::memory_order_relaxed);
        if (step < 16)
        {
            for (int k = 0; k < step; ++k) if (learnNotes_[k].load(std::memory_order_relaxed) == note) return true;
            learnNotes_[step].store(note, std::memory_order_relaxed);
            learnStep_.store(step + 1, std::memory_order_release);
        }
        return true;
    }
    // worker: when 16 notes are in, take them and save
    bool finishIfDone()
    {
        if (!learning_.load() || learnStep_.load() < 16) return false;
        int n[16];
        for (int k = 0; k < 16; ++k) n[k] = learnNotes_[k].load();
        setLearned(n);
        learning_.store(false);
        save();
        return true;
    }
    std::string info() const
    {
        if (learning_.load()) { int s = learnStep_.load(); return s < 16 ? "Now press pad " + std::to_string(s + 1) + " ..." : std::string("done"); }
        return learned_.load() ? "learned, pad 1 = note " + std::to_string(noteOf(0)) : std::string("default, pad 1 = note 36");
    }

private:
    std::atomic<int8_t> noteToPad_[128];
    std::atomic<bool> learned_{false}, learning_{false};
    std::atomic<int> learnStep_{0};
    std::atomic<int> learnNotes_[16];
};

// ---------------------------------------------------------------- WAV (for SCHLAG and the previews)
struct Wave
{
    std::vector<float> L, R; // R empty: mono
    float rate = 44100.f;
    size_t frames() const { return L.size(); }
};

inline bool readWav(const std::vector<uint8_t>& b, Wave& out, float maxSeconds)
{
    auto u16 = [&](size_t i) { return (uint32_t)b[i] | ((uint32_t)b[i + 1] << 8); };
    auto u32 = [&](size_t i) { return u16(i) | (u16(i + 2) << 16); };
    if (b.size() < 44 || std::memcmp(b.data(), "RIFF", 4) || std::memcmp(b.data() + 8, "WAVE", 4)) return false;
    size_t pos = 12, dataAt = 0, dataLen = 0;
    int fmt = 0, ch = 0, bits = 0;
    uint32_t rate = 0;
    while (pos + 8 <= b.size())
    {
        uint32_t len = u32(pos + 4);
        if (!std::memcmp(b.data() + pos, "fmt ", 4) && pos + 24 <= b.size())
        {
            fmt = (int)u16(pos + 8); ch = (int)u16(pos + 10); rate = u32(pos + 12); bits = (int)u16(pos + 22);
            if (fmt == 0xFFFE && len >= 40 && pos + 34 <= b.size()) fmt = (int)u16(pos + 32);
        }
        else if (!std::memcmp(b.data() + pos, "data", 4)) { dataAt = pos + 8; dataLen = std::min<size_t>(len, b.size() - dataAt); break; }
        pos += 8 + len + (len & 1);
    }
    if (!dataAt || ch < 1 || rate < 4000 || !(fmt == 1 || fmt == 3)) return false;
    int bytes = bits / 8;
    if (bytes < 1 || bytes > 8 || (fmt == 3 && bytes != 4 && bytes != 8)) return false;
    size_t frames = dataLen / ((size_t)bytes * ch);
    frames = std::min(frames, (size_t)(maxSeconds * rate));
    out.rate = (float)rate;
    out.L.resize(frames);
    if (ch > 1) out.R.resize(frames); else out.R.clear();
    for (size_t f = 0; f < frames; ++f)
        for (int c = 0; c < std::min(ch, 2); ++c)
        {
            const uint8_t* p = b.data() + dataAt + (f * ch + c) * bytes;
            float v = 0.f;
            if (fmt == 3) { if (bytes == 4) { float x; std::memcpy(&x, p, 4); v = x; } else { double x; std::memcpy(&x, p, 8); v = (float)x; } }
            else if (bytes == 1) v = (p[0] - 128) / 128.f;
            else if (bytes == 2) v = (int16_t)(p[0] | (p[1] << 8)) / 32768.f;
            else if (bytes == 3) v = (int32_t)((p[0] << 8) | (p[1] << 16) | (p[2] << 24)) / 2147483648.f;
            else v = (int32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24)) / 2147483648.f;
            (c == 0 ? out.L : out.R)[f] = v;
        }
    return frames > 0;
}

inline bool loadWav(const std::string& path, Wave& out, float maxSeconds)
{
    std::vector<uint8_t> bytes;
    return mpc::readFile(path, bytes) && readWav(bytes, out, maxSeconds);
}

// ---------------------------------------------------------------- PRINT: MIDI files
struct MidiNote { double step; double len; int note; int vel; }; // step / len in 16ths

// A standard MIDI file (type 0, one track, 96 ticks per quarter) with tempo, 4/4 and the notes.
inline std::vector<uint8_t> midiFile(const std::vector<MidiNote>& notes, double bpm, const std::string& name, double loopSteps)
{
    const int ppq = 96, tps = ppq / 4; // ticks per 16th
    struct Ev { long tick; int order; uint8_t a, b, c; };
    std::vector<Ev> ev;
    for (const MidiNote& n : notes)
    {
        if (n.note < 0 || n.note > 127) continue;
        long on = std::max(0L, std::lround(n.step * tps));
        long off = std::max(on + 1, std::lround((n.step + n.len) * tps));
        off = std::min(off, std::lround(loopSteps * tps));
        if (off <= on) continue;
        ev.push_back({on, 1, 0x90, (uint8_t)n.note, (uint8_t)std::max(1, std::min(127, n.vel))});
        ev.push_back({off, 0, 0x80, (uint8_t)n.note, 0});
    }
    std::sort(ev.begin(), ev.end(), [](const Ev& x, const Ev& y) { return x.tick != y.tick ? x.tick < y.tick : x.order < y.order; });
    std::vector<uint8_t> t;
    auto vlq = [&t](uint32_t v) {
        uint8_t buf[5]; int n = 0;
        buf[n++] = v & 0x7f;
        while ((v >>= 7)) buf[n++] = (uint8_t)(0x80 | (v & 0x7f));
        while (n) t.push_back(buf[--n]);
    };
    // track name, tempo, time signature
    vlq(0); t.push_back(0xFF); t.push_back(0x03); vlq((uint32_t)name.size()); t.insert(t.end(), name.begin(), name.end());
    uint32_t us = (uint32_t)std::lround(60000000.0 / std::max(20.0, bpm));
    vlq(0); t.insert(t.end(), {0xFF, 0x51, 0x03, (uint8_t)(us >> 16), (uint8_t)(us >> 8), (uint8_t)us});
    vlq(0); t.insert(t.end(), {0xFF, 0x58, 0x04, 4, 2, 24, 8});
    long last = 0;
    for (const Ev& e : ev) { vlq((uint32_t)(e.tick - last)); last = e.tick; t.push_back(e.a); t.push_back(e.b); t.push_back(e.c); }
    long end = std::lround(loopSteps * tps);
    vlq((uint32_t)std::max(0L, end - last)); t.insert(t.end(), {0xFF, 0x2F, 0x00});
    std::vector<uint8_t> f = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, (uint8_t)(ppq >> 8), (uint8_t)ppq, 'M', 'T', 'r', 'k'};
    uint32_t len = (uint32_t)t.size();
    f.push_back((uint8_t)(len >> 24)); f.push_back((uint8_t)(len >> 16)); f.push_back((uint8_t)(len >> 8)); f.push_back((uint8_t)len);
    f.insert(f.end(), t.begin(), t.end());
    return f;
}

// The print folder: what the user set, else Genra/PRINT.
inline std::string printDir(const std::string& chosen)
{
    if (!chosen.empty() && isDir(chosen)) return chosen;
    std::string g = genraDir();
    if (g.empty()) return std::string();
    std::string d = g + "/PRINT";
    ::mkdir(d.c_str(), 0777);
    return isDir(d) ? d : g;
}

// "Print 07 ..." -> 7
inline int printNumber(const std::string& file)
{
    if (file.compare(0, 6, "Print ") != 0) return 0;
    return std::atoi(file.c_str() + 6);
}

// the prints in a folder, newest (highest number) first
inline std::vector<std::string> listPrints(const std::string& dir)
{
    std::vector<std::string> v = listDir(dir, false, ".mid");
    std::stable_sort(v.begin(), v.end(), [](const std::string& a, const std::string& b) { return printNumber(a) > printNumber(b); });
    return v;
}

// The number for a new print. All plugins share one counter per loop: the same loop (LINK id) gets the same number,
// so "Print 07 Chop", "Print 07 Bass", "Print 07 Drums" belong together.
inline int printNumberFor(const std::string& dir, uint32_t loopId)
{
    Settings s("print.txt");
    s.load();
    int maxNo = 0;
    for (const std::string& f : listDir(dir, false, ".mid")) maxNo = std::max(maxNo, printNumber(f));
    std::string lastId = s.get("loop"), lastNo = s.get("number");
    int n;
    if (!lastId.empty() && (uint32_t)std::strtoul(lastId.c_str(), nullptr, 10) == loopId && std::atoi(lastNo.c_str()) > 0)
        n = std::atoi(lastNo.c_str());
    else
        n = std::max(maxNo, std::atoi(lastNo.c_str())) + 1;
    s.set("loop", std::to_string(loopId));
    s.set("number", std::to_string(n));
    s.save();
    return n;
}

// ---------------------------------------------------------------- the tap browser (rows of a folder)
class Browser
{
public:
    struct Entry { std::string label, path; int kind; }; // kind: 0 up, 1 folder, 2 file, 3 drive
    int rows = 10;
    const char* extra = nullptr; // a second kind of file to list after the WAVs (e.g. ".kit")

    void open(const std::string& dir) { cwd_ = (!dir.empty() && isDir(dir)) ? dir : std::string(); list(); }
    void drives() { cwd_.clear(); list(); }
    const std::string& cwd() const { return cwd_; }
    void list(const std::string& select = std::string())
    {
        entries_.clear();
        if (cwd_.empty() || cwd_ == "*")
        {
            if (cwd_ == "*") entries_.push_back({"..", "", 0});
            for (const std::string& d : findDrives(cwd_ == "*")) entries_.push_back({pathLabel(d), d, 3});
            if (cwd_.empty()) entries_.push_back({"(all mounts)", "*", 1});
        }
        else
        {
            entries_.push_back({"..", "", 0});
            for (const std::string& n : listDir(cwd_, true)) entries_.push_back({n, cwd_ + "/" + n, 1});
            for (const std::string& n : listDir(cwd_, false)) entries_.push_back({n, cwd_ + "/" + n, 2});
            if (extra)
                for (const std::string& n : listDir(cwd_, false, extra)) entries_.push_back({n, cwd_ + "/" + n, 2});
        }
        top_ = 0;
        if (!select.empty())
            for (size_t i = 0; i < entries_.size(); ++i)
                if (entries_[i].path == select) { top_ = std::max(0, (int)i - rows / 2); break; }
        clampTop();
    }
    void clampTop() { top_ = std::max(0, std::min(top_, std::max(0, (int)entries_.size() - rows))); }
    void scroll(int d) { top_ += d; clampTop(); }
    void page(int d) { top_ += d * rows; clampTop(); }
    void up()
    {
        if (cwd_.empty()) return;
        std::string from = cwd_;
        if (cwd_ == "*") cwd_.clear();
        else
        {
            bool isDrive = false;
            for (const std::string& d : findDrives(false)) if (cwd_ == d) isDrive = true;
            size_t i = cwd_.find_last_of('/');
            cwd_ = isDrive || i == std::string::npos || i == 0 ? std::string() : cwd_.substr(0, i);
        }
        list(from);
    }
    // a tap on a row: folders open (returns empty), a file returns its path
    std::string tap(int row)
    {
        int i = top_ + row;
        if (i < 0 || i >= (int)entries_.size()) return std::string();
        Entry e = entries_[(size_t)i];
        if (e.kind == 0) { up(); return std::string(); }
        if (e.kind == 1 || e.kind == 3) { cwd_ = e.path; list(); return std::string(); }
        return e.path;
    }
    // the text of row r: marks: "> " the loaded file, "» " the file being previewed
    std::string rowText(int r, const std::string& loaded, const std::string& preview) const
    {
        size_t i = (size_t)(top_ + r);
        if (i >= entries_.size()) return std::string();
        const Entry& e = entries_[i];
        if (e.kind == 0) return "..  (back)";
        if (e.kind == 1 || e.kind == 3) return "[" + e.label + "]";
        if (extra && hasExt(e.label, extra)) return "KIT  " + e.label.substr(0, e.label.size() - std::strlen(extra));
        if (!preview.empty() && e.path == preview) return "» " + stripWav(e.label);
        return ((!loaded.empty() && e.path == loaded) ? "> " : "") + stripWav(e.label);
    }
    bool isSelected(int r, const std::string& path) const
    {
        size_t i = (size_t)(top_ + r);
        return i < entries_.size() && !path.empty() && entries_[i].path == path;
    }
    std::string pathText() const { return cwd_.empty() ? std::string("Drives") : pathLabel(cwd_); }
    std::string info() const
    {
        int n = (int)entries_.size();
        if (!n) return "empty";
        char b[64];
        std::snprintf(b, sizeof b, "Rows %d-%d of %d", top_ + 1, std::min(n, top_ + rows), n);
        return b;
    }

private:
    std::vector<Entry> entries_;
    std::string cwd_;
    int top_ = 0;
};

} // namespace genrafiles

#endif
