// genra_diag.h - DIAG: a log of what the MPC offers and does, for finding a way into its sequencer.
//
// SPDX-License-Identifier: GPL-3.0-only
//
// One press of DIAG (GENRA CHOP's SETUP page) starts a capture of about two minutes and writes a text file to
// <drive>/Genra/logs/. It only reads, it changes nothing on the MPC:
//   1. the system (OS, kernel), the ALSA cards and sequencer clients / ports with their capabilities
//   2. the running programs and the network ports that listen (a remote control or API?)
//   3. every MIDI event the MPC's own ports send while you play, stop, record or use the step sequencer
//      (an own client "GENRA DIAG" subscribes to every readable port); clock and active sensing are counted
//   4. the host transport the plugin sees (playing, recording, position, sample position, tempo), on each change and
//      often during the first seconds after a start (the count-in)
// libasound is loaded at run time (dlopen) like genra_midiout.h. The capture runs on its own thread.

#pragma once

#include <dirent.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace genradiag {

// what the audio thread publishes each block (written there, read by the capture)
struct Transport
{
    std::atomic<double> ppq{0.0}, samplePos{0.0}, tempo{0.0}, barStart{0.0};
    std::atomic<int> flags{0}; // 1 valid, 2 playing, 4 recording, 8 ppq valid, 16 cycling
    std::atomic<long> blocks{0};
};

class Diag
{
public:
    ~Diag() { stop(); }

    bool running() const { return running_.load(); }
    int secondsLeft() const { return left_.load(); }
    long events() const { return events_.load(); }
    std::string lastFile() const { std::lock_guard<std::mutex> l(m_); return file_; }
    std::string error() const { std::lock_guard<std::mutex> l(m_); return error_; }

    // dir: the Genra folder; head: lines about the plugin (version, ports); tr: the host transport
    void start(const std::string& dir, const std::string& head, const Transport* tr, int seconds = 120)
    {
        if (running_.load()) return;
        stop();
        running_.store(true);
        events_.store(0);
        left_.store(seconds);
        th_ = std::thread([this, dir, head, tr, seconds] { run(dir, head, tr, seconds); });
    }
    void stop()
    {
        quit_.store(true);
        if (th_.joinable()) th_.join();
        quit_.store(false);
    }

private:
    // ---------------------------------------------------------------- the capture
    void run(std::string dir, std::string head, const Transport* tr, int seconds)
    {
        std::string logs = dir + "/logs";
        ::mkdir(logs.c_str(), 0777);
        char name[96];
        int n = 1;
        for (; n < 1000; ++n)
        {
            std::snprintf(name, sizeof name, "%s/diag-%03d.txt", logs.c_str(), n);
            struct stat st;
            if (::stat(name, &st) != 0) break;
        }
        FILE* f = std::fopen(name, "w");
        if (!f)
        {
            std::lock_guard<std::mutex> l(m_);
            error_ = "cannot write " + logs;
            running_.store(false);
            return;
        }
        {
            std::lock_guard<std::mutex> l(m_);
            file_ = name;
            error_.clear();
        }
        out_ = f;
        t0_ = std::chrono::steady_clock::now();
        line("# GENRA DIAG");
        line(head);
        std::time_t now = std::time(nullptr);
        line(std::string("time (MPC clock): ") + std::ctime(&now));
        section("system");
        dumpFile("/etc/os-release", 40);
        dumpFile("/proc/version", 5);
        dumpFile("/proc/cpuinfo", 12);
        section("ALSA");
        dumpFile("/proc/asound/cards", 40);
        dumpFile("/proc/asound/devices", 60);
        dumpFile("/proc/asound/seq/clients", 300);
        section("programs");
        programs();
        section("network: listening ports");
        listening("/proc/net/tcp", "tcp", true);
        listening("/proc/net/tcp6", "tcp6", true);
        listening("/proc/net/udp", "udp", false);
        listening("/proc/net/udp6", "udp6", false);
        section("unix sockets (with a path)");
        unixSockets();
        std::fflush(f);

        section("sequencer clients and ports (API)");
        bool seq = openSeq();
        if (seq) listPorts(true);
        section("capture: MIDI from every readable port + host transport (" + std::to_string(seconds) + " s)");
        line("Play, Stop, Rec / Overdub with count-in, the step sequencer, pads, Q-Links: everything the MPC sends shows below.");
        capture(tr, seconds, seq);
        if (seq) closeSeq();
        section("end");
        std::fclose(f);
        out_ = nullptr;
        left_.store(0);
        running_.store(false);
    }

    void capture(const Transport* tr, int seconds, bool seq)
    {
        using clk = std::chrono::steady_clock;
        const auto end = clk::now() + std::chrono::seconds(seconds);
        int lastFlags = -1;
        double lastPpq = 0.0;
        auto startAt = clk::now() - std::chrono::hours(1);
        auto lastRow = clk::now();
        auto lastCount = clk::now();
        long clocks = 0, sensing = 0, ticks = 0;
        while (!quit_.load() && clk::now() < end)
        {
            left_.store((int)std::chrono::duration_cast<std::chrono::seconds>(end - clk::now()).count());
            if (seq) readEvents(clocks, sensing, ticks);
            if (tr)
            {
                const int fl = tr->flags.load();
                const double q = tr->ppq.load();
                const bool playing = (fl & 2) != 0;
                const auto t = clk::now();
                if (playing && !(lastFlags & 2)) startAt = t;
                const bool early = std::chrono::duration_cast<std::chrono::milliseconds>(t - startAt).count() < 8000;
                const long gap = (long)std::chrono::duration_cast<std::chrono::milliseconds>(t - lastRow).count();
                const bool jump = playing && (lastFlags & 2) && q < lastPpq - 0.25;
                if (fl != lastFlags || jump || (playing && early && gap >= 50) || (playing && gap >= 1000))
                {
                    char b[200];
                    std::snprintf(b, sizeof b, "TRANSPORT %s%s%s%s ppq %.3f smp %.0f bar %.2f bpm %.2f%s",
                                  (fl & 2) ? "PLAY" : "stop", (fl & 4) ? " REC" : "", (fl & 16) ? " CYCLE" : "",
                                  (fl & 8) ? "" : " (no ppq)", q, tr->samplePos.load(), tr->barStart.load(), tr->tempo.load(),
                                  jump ? "  << JUMP BACK" : "");
                    line(b);
                    lastRow = t;
                }
                lastFlags = fl;
                lastPpq = q;
            }
            if (clocks || sensing || ticks)
            {
                const auto t = clk::now();
                if (std::chrono::duration_cast<std::chrono::milliseconds>(t - lastCount).count() >= 1000)
                {
                    char b[120];
                    std::snprintf(b, sizeof b, "(last second: %ld clock, %ld tick, %ld active sensing)", clocks, ticks, sensing);
                    line(b);
                    clocks = sensing = ticks = 0;
                    lastCount = t;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
        }
    }

    // ---------------------------------------------------------------- text
    void line(const std::string& s)
    {
        if (!out_) return;
        const long ms = (long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0_).count();
        std::fprintf(out_, "%8.3f  %s\n", ms / 1000.0, s.c_str());
    }
    void section(const std::string& s)
    {
        if (!out_) return;
        std::fprintf(out_, "\n==== %s ====\n", s.c_str());
        std::fflush(out_);
    }
    void dumpFile(const char* path, int maxLines)
    {
        FILE* f = std::fopen(path, "r");
        if (!f) { line(std::string(path) + ": (not readable)"); return; }
        std::fprintf(out_, "-- %s\n", path);
        char b[512];
        int n = 0;
        while (n < maxLines && std::fgets(b, sizeof b, f)) { std::fputs(b, out_); ++n; }
        if (n >= maxLines) std::fprintf(out_, "   (...)\n");
        std::fclose(f);
    }
    static std::string readSmall(const std::string& path, size_t max)
    {
        std::string s;
        FILE* f = std::fopen(path.c_str(), "r");
        if (!f) return s;
        char b[256];
        size_t r;
        while (s.size() < max && (r = std::fread(b, 1, sizeof b, f)) > 0) s.append(b, r);
        std::fclose(f);
        if (s.size() > max) s.resize(max);
        for (char& c : s) if (c == '\0') c = ' ';
        while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back();
        return s;
    }
    void programs()
    {
        DIR* d = ::opendir("/proc");
        if (!d) { line("/proc: (not readable)"); return; }
        int n = 0;
        while (struct dirent* e = ::readdir(d))
        {
            const char* nm = e->d_name;
            if (nm[0] < '0' || nm[0] > '9') continue;
            std::string base = std::string("/proc/") + nm;
            std::string cmd = readSmall(base + "/cmdline", 160);
            if (cmd.empty()) continue; // kernel threads
            std::fprintf(out_, "%6s  %s\n", nm, cmd.c_str());
            if (++n >= 300) break;
        }
        ::closedir(d);
    }
    void listening(const char* path, const char* proto, bool tcp)
    {
        FILE* f = std::fopen(path, "r");
        if (!f) return;
        char b[512];
        std::fgets(b, sizeof b, f); // header
        while (std::fgets(b, sizeof b, f))
        {
            char local[80] = {0}, remote[80] = {0}, st[8] = {0};
            unsigned long inode = 0;
            if (std::sscanf(b, "%*d: %79s %79s %7s %*s %*s %*s %*s %*s %lu", local, remote, st, &inode) < 3) continue;
            if (tcp && std::strcmp(st, "0A") != 0) continue;  // LISTEN
            if (!tcp && std::strcmp(st, "07") != 0) continue; // UDP: bound
            const char* colon = std::strrchr(local, ':');
            unsigned port = colon ? (unsigned)std::strtoul(colon + 1, nullptr, 16) : 0;
            std::fprintf(out_, "%s port %u (local %s, inode %lu)\n", proto, port, local, inode);
        }
        std::fclose(f);
    }
    void unixSockets()
    {
        FILE* f = std::fopen("/proc/net/unix", "r");
        if (!f) return;
        char b[512];
        std::fgets(b, sizeof b, f);
        int n = 0;
        while (std::fgets(b, sizeof b, f) && n < 120)
        {
            const char* p = std::strchr(b, '/');
            const char* at = std::strchr(b, '@');
            if (!p && !at) continue;
            std::fputs(p ? p : at, out_);
            ++n;
        }
        std::fclose(f);
    }

    // ---------------------------------------------------------------- ALSA sequencer
    struct Ev
    {
        uint8_t type, flags, tag, queue;
        uint32_t timeA, timeB;
        uint8_t srcClient, srcPort, dstClient, dstPort;
        uint8_t data[12];
    };
    static_assert(sizeof(Ev) == 28, "snd_seq_event_t is 28 bytes");

    template <class T> bool sym(T& f, const char* n) { f = (T)dlsym(lib_, n); return f != nullptr; }

    bool openSeq()
    {
        lib_ = dlopen("libasound.so.2", RTLD_NOW | RTLD_LOCAL);
        if (!lib_) lib_ = dlopen("libasound.so", RTLD_NOW | RTLD_LOCAL);
        if (!lib_) { line("libasound: not found"); return false; }
        bool ok = sym(seqOpen_, "snd_seq_open") && sym(seqClose_, "snd_seq_close") && sym(setName_, "snd_seq_set_client_name") &&
                  sym(mkPort_, "snd_seq_create_simple_port") && sym(clientId_, "snd_seq_client_id") &&
                  sym(input_, "snd_seq_event_input") && sym(connectFrom_, "snd_seq_connect_from") &&
                  sym(ciMalloc_, "snd_seq_client_info_malloc") && sym(ciFree_, "snd_seq_client_info_free") &&
                  sym(ciSetClient_, "snd_seq_client_info_set_client") && sym(ciGetClient_, "snd_seq_client_info_get_client") &&
                  sym(ciGetName_, "snd_seq_client_info_get_name") && sym(ciGetType_, "snd_seq_client_info_get_type") &&
                  sym(nextClient_, "snd_seq_query_next_client") && sym(piMalloc_, "snd_seq_port_info_malloc") &&
                  sym(piFree_, "snd_seq_port_info_free") && sym(piSetClient_, "snd_seq_port_info_set_client") &&
                  sym(piSetPort_, "snd_seq_port_info_set_port") && sym(piGetPort_, "snd_seq_port_info_get_port") &&
                  sym(piGetName_, "snd_seq_port_info_get_name") && sym(piGetCap_, "snd_seq_port_info_get_capability") &&
                  sym(piGetType_, "snd_seq_port_info_get_type") && sym(nextPort_, "snd_seq_query_next_port");
        if (!ok) { line("libasound: functions missing"); return false; }
        if (seqOpen_(&seq_, "default", 3 /* duplex */, 1 /* nonblock */) < 0 || !seq_) { line("ALSA sequencer: cannot open"); seq_ = nullptr; return false; }
        setName_(seq_, "GENRA DIAG");
        port_ = mkPort_(seq_, "GENRA DIAG IN", (1u << 1) | (1u << 6) /* write, subs write */, (1u << 1) | (1u << 20));
        me_ = clientId_(seq_);
        char b[80];
        std::snprintf(b, sizeof b, "own client GENRA DIAG %d:%d", me_, port_);
        line(b);
        return port_ >= 0;
    }
    void closeSeq()
    {
        if (seq_ && seqClose_) seqClose_(seq_);
        seq_ = nullptr;
        if (lib_) dlclose(lib_);
        lib_ = nullptr;
    }
    static std::string caps(unsigned c)
    {
        std::string s;
        const char* n[] = {"read", "write", "sync-read", "sync-write", "duplex", "subs-read", "subs-write", "no-export"};
        for (int i = 0; i < 8; ++i)
            if (c & (1u << i)) s += std::string(s.empty() ? "" : ",") + n[i];
        return s;
    }
    static std::string types(unsigned t)
    {
        std::string s;
        struct { unsigned bit; const char* n; } tt[] = {{1u << 0, "specific"}, {1u << 1, "midi-generic"}, {1u << 2, "gm"},
            {1u << 3, "gs"}, {1u << 4, "xg"}, {1u << 5, "mt32"}, {1u << 6, "gm2"}, {1u << 10, "synth"}, {1u << 11, "direct-sample"},
            {1u << 12, "sample"}, {1u << 16, "hardware"}, {1u << 17, "software"}, {1u << 18, "synthesizer"}, {1u << 19, "port"},
            {1u << 20, "application"}};
        for (auto& x : tt)
            if (t & x.bit) s += std::string(s.empty() ? "" : ",") + x.n;
        return s;
    }
    void listPorts(bool subscribe)
    {
        void* ci = nullptr;
        void* pi = nullptr;
        ciMalloc_(&ci);
        piMalloc_(&pi);
        ciSetClient_(ci, -1);
        int subs = 0;
        while (nextClient_(seq_, ci) >= 0)
        {
            const int c = ciGetClient_(ci);
            char b[300];
            std::snprintf(b, sizeof b, "client %d '%s' (%s)", c, ciGetName_(ci), ciGetType_(ci) == 2 ? "kernel" : "user");
            line(b);
            piSetClient_(pi, c);
            piSetPort_(pi, -1);
            while (nextPort_(seq_, pi) >= 0)
            {
                const int p = piGetPort_(pi);
                const unsigned cap = piGetCap_(pi);
                std::string did;
                if (subscribe && c != me_ && (cap & 1u) && (cap & (1u << 5)) && !(c == 0 && p == 0))
                    did = connectFrom_(seq_, port_, c, p) >= 0 ? "  -> listening" : "  -> cannot listen";
                if (!did.empty() && did.find("listening") != std::string::npos) ++subs;
                std::snprintf(b, sizeof b, "   port %d:%d '%s'  caps [%s]  type [%s]%s", c, p, piGetName_(pi), caps(cap).c_str(),
                              types(piGetType_(pi)).c_str(), did.c_str());
                line(b);
            }
        }
        char b[80];
        std::snprintf(b, sizeof b, "listening to %d ports", subs);
        line(b);
        piFree_(pi);
        ciFree_(ci);
    }
    static const char* typeName(int t)
    {
        switch (t)
        {
            case 5: return "NOTE"; case 6: return "NOTE ON"; case 7: return "NOTE OFF"; case 8: return "POLY PRESSURE";
            case 10: return "CC"; case 11: return "PROGRAM"; case 12: return "CHAN PRESSURE"; case 13: return "PITCH BEND";
            case 14: return "CC14"; case 15: return "NRPN"; case 16: return "RPN"; case 20: return "SONG POSITION";
            case 21: return "SONG SELECT"; case 22: return "MTC QUARTER FRAME"; case 23: return "TIME SIGNATURE";
            case 24: return "KEY SIGNATURE"; case 30: return "START"; case 31: return "CONTINUE"; case 32: return "STOP";
            case 33: return "SETPOS TICK"; case 34: return "SETPOS TIME"; case 35: return "TEMPO"; case 36: return "CLOCK";
            case 37: return "TICK"; case 40: return "TUNE REQUEST"; case 41: return "RESET"; case 42: return "ACTIVE SENSING";
            case 60: return "CLIENT START"; case 61: return "CLIENT EXIT"; case 62: return "CLIENT CHANGE";
            case 63: return "PORT START"; case 64: return "PORT EXIT"; case 65: return "PORT CHANGE";
            case 66: return "PORT SUBSCRIBED"; case 67: return "PORT UNSUBSCRIBED"; case 130: return "SYSEX";
            default: return "EVENT";
        }
    }
    void readEvents(long& clocks, long& sensing, long& ticks)
    {
        for (int guard = 0; guard < 256; ++guard)
        {
            void* p = nullptr;
            if (input_(seq_, &p) < 0 || !p) return;
            Ev e;
            std::memcpy(&e, p, sizeof e);
            if (e.type == 36) { ++clocks; continue; }
            if (e.type == 42) { ++sensing; continue; }
            if (e.type == 37) { ++ticks; continue; }
            events_.fetch_add(1);
            char b[400];
            int n = std::snprintf(b, sizeof b, "MIDI from %d:%d  %s (%d)", e.srcClient, e.srcPort, typeName(e.type), e.type);
            const uint8_t* d = e.data;
            if (e.type >= 5 && e.type <= 8)
                n += std::snprintf(b + n, sizeof b - n, "  ch %d note %d vel %d", d[0] + 1, d[1], d[2]);
            else if (e.type >= 10 && e.type <= 16)
            {
                int32_t param, value;
                std::memcpy(&param, d + 4, 4);
                std::memcpy(&value, d + 8, 4);
                n += std::snprintf(b + n, sizeof b - n, "  ch %d param %d value %d", d[0] + 1, param, value);
            }
            else if (e.type >= 20 && e.type <= 35)
            {
                int32_t value;
                std::memcpy(&value, d + 4, 4);
                n += std::snprintf(b + n, sizeof b - n, "  value %d  (queue data %d:%d)", value, d[0], d[1]);
            }
            else if (e.type >= 60 && e.type <= 67)
                n += std::snprintf(b + n, sizeof b - n, "  addr %d:%d -> %d:%d", d[0], d[1], d[2], d[3]);
            else if (e.type == 130)
            {
                uint32_t len;
                std::memcpy(&len, d, 4);
                const uint8_t* ptr = nullptr;
                std::memcpy(&ptr, d + 4, sizeof(ptr) <= 8 ? sizeof(ptr) : 8);
                n += std::snprintf(b + n, sizeof b - n, "  %u bytes:", len);
                for (uint32_t i = 0; ptr && i < len && i < 48 && n < (int)sizeof b - 4; ++i)
                    n += std::snprintf(b + n, sizeof b - n, " %02X", ptr[i]);
                if (len > 48) n += std::snprintf(b + n, sizeof b - n, " ...");
            }
            else
            {
                for (int i = 0; i < 12; ++i) n += std::snprintf(b + n, sizeof b - n, " %02X", d[i]);
            }
            line(b);
        }
    }

    typedef int (*SeqOpen)(void**, const char*, int, int);
    typedef int (*SeqClose)(void*);
    typedef int (*SetName)(void*, const char*);
    typedef int (*MkPort)(void*, const char*, unsigned int, unsigned int);
    typedef int (*ClientId)(void*);
    typedef int (*Input)(void*, void**);
    typedef int (*ConnectFrom)(void*, int, int, int);
    typedef int (*Malloc)(void**);
    typedef void (*Free)(void*);
    typedef void (*SetInt)(void*, int);
    typedef int (*GetInt)(const void*);
    typedef unsigned (*GetUInt)(const void*);
    typedef const char* (*GetStr)(const void*);
    typedef int (*Next)(void*, void*);

    void* lib_ = nullptr;
    void* seq_ = nullptr;
    int port_ = -1, me_ = -1;
    SeqOpen seqOpen_ = nullptr;
    SeqClose seqClose_ = nullptr;
    SetName setName_ = nullptr;
    MkPort mkPort_ = nullptr;
    ClientId clientId_ = nullptr;
    Input input_ = nullptr;
    ConnectFrom connectFrom_ = nullptr;
    Malloc ciMalloc_ = nullptr, piMalloc_ = nullptr;
    Free ciFree_ = nullptr, piFree_ = nullptr;
    SetInt ciSetClient_ = nullptr, piSetClient_ = nullptr, piSetPort_ = nullptr;
    GetInt ciGetClient_ = nullptr, ciGetType_ = nullptr, piGetPort_ = nullptr;
    GetUInt piGetCap_ = nullptr, piGetType_ = nullptr;
    GetStr ciGetName_ = nullptr, piGetName_ = nullptr;
    Next nextClient_ = nullptr, nextPort_ = nullptr;

    FILE* out_ = nullptr;
    std::chrono::steady_clock::time_point t0_;
    std::thread th_;
    std::atomic<bool> running_{false}, quit_{false};
    std::atomic<int> left_{0};
    std::atomic<long> events_{0};
    mutable std::mutex m_;
    std::string file_, error_;
};

} // namespace genradiag
