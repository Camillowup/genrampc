// genra_midiout.h - a GENRA plugin's own MIDI output: an ALSA sequencer port the MPC can take as a MIDI input.
//
// SPDX-License-Identifier: GPL-3.0-only
//
// The MPC drops the MIDI a plugin hands to the host, so a plugin cannot record into the MPC's sequence. The
// community workaround: open an ALSA sequencer client with a readable port ("GENRA CHOP OUT"); the MPC lists it as
// a MIDI input, and a track whose input is that port records the notes while Rec is armed.
// libasound is loaded at run time (dlopen), so the plugin needs no ALSA headers and still loads where there is no
// ALSA: then the port simply is not there and status() says why.
// The event struct below is ALSA's snd_seq_event_t (28 bytes), filled by hand.
// Thread: open() from the worker; noteOn / noteOff / allOff from process() (non-blocking, direct output).

#pragma once

#include <dlfcn.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace genramidi {

class Out
{
public:
    ~Out() { close(); }

    // name: the client's name; the port is "<name> OUT"
    bool open(const std::string& name)
    {
        if (ok_.load()) return true;
        lib_ = dlopen("libasound.so.2", RTLD_NOW | RTLD_LOCAL);
        if (!lib_) lib_ = dlopen("libasound.so", RTLD_NOW | RTLD_LOCAL);
        if (!lib_) { status_ = "no libasound"; return false; }
        seqOpen_ = (SeqOpen)dlsym(lib_, "snd_seq_open");
        seqClose_ = (SeqClose)dlsym(lib_, "snd_seq_close");
        setName_ = (SetName)dlsym(lib_, "snd_seq_set_client_name");
        mkPort_ = (MkPort)dlsym(lib_, "snd_seq_create_simple_port");
        clientId_ = (ClientId)dlsym(lib_, "snd_seq_client_id");
        outDirect_ = (OutDirect)dlsym(lib_, "snd_seq_event_output_direct");
        if (!seqOpen_ || !setName_ || !mkPort_ || !outDirect_) { status_ = "libasound incomplete"; return false; }
        if (seqOpen_(&seq_, "default", kOpenOutput, kNonBlock) < 0 || !seq_) { status_ = "no ALSA sequencer"; return false; }
        setName_(seq_, name.c_str());
        port_ = mkPort_(seq_, (name + " OUT").c_str(), kCapRead | kCapSubsRead, kTypeMidiGeneric | kTypeApplication);
        if (port_ < 0) { status_ = "no port"; return false; }
        client_ = clientId_ ? clientId_(seq_) : -1;
        char b[64];
        std::snprintf(b, sizeof b, "%s OUT %d:%d", name.c_str(), client_, port_);
        status_ = b;
        ok_.store(true);
        return true;
    }

    void close()
    {
        ok_.store(false);
        if (seq_ && seqClose_) seqClose_(seq_);
        seq_ = nullptr;
        if (lib_) dlclose(lib_);
        lib_ = nullptr;
    }

    bool ok() const { return ok_.load(); }
    const std::string& status() const { return status_; }

    void noteOn(int note, int vel, int ch = 0) { send(kNoteOn, note, vel, ch); held_[ch & 15][note & 127] = true; }
    void noteOff(int note, int ch = 0) { send(kNoteOff, note, 0, ch); held_[ch & 15][note & 127] = false; }
    // every note still held, on every channel
    void allOff()
    {
        for (int c = 0; c < 16; ++c)
            for (int n = 0; n < 128; ++n)
                if (held_[c][n]) noteOff(n, c);
    }
    long sent() const { return sent_; }

private:
    struct Ev
    {
        uint8_t type, flags, tag, queue;
        uint32_t timeA, timeB;
        uint8_t srcClient, srcPort, dstClient, dstPort;
        uint8_t channel, note, velocity, offVelocity;
        uint32_t duration;
        uint32_t pad;
    };
    static_assert(sizeof(Ev) == 28, "snd_seq_event_t is 28 bytes");

    void send(uint8_t type, int note, int vel, int ch)
    {
        if (!ok_.load()) return;
        Ev e;
        std::memset(&e, 0, sizeof e);
        e.type = type;
        e.queue = kQueueDirect;
        e.srcPort = (uint8_t)port_;
        e.dstClient = kAddrSubscribers;
        e.dstPort = kAddrUnknown;
        e.channel = (uint8_t)(ch & 15);
        e.note = (uint8_t)(note & 127);
        e.velocity = (uint8_t)(vel < 0 ? 0 : (vel > 127 ? 127 : vel));
        if (outDirect_(seq_, &e) >= 0) ++sent_;
    }

    typedef int (*SeqOpen)(void**, const char*, int, int);
    typedef int (*SeqClose)(void*);
    typedef int (*SetName)(void*, const char*);
    typedef int (*MkPort)(void*, const char*, unsigned int, unsigned int);
    typedef int (*ClientId)(void*);
    typedef int (*OutDirect)(void*, void*);

    static const int kOpenOutput = 1, kNonBlock = 1;
    static const unsigned kCapRead = 1u << 0, kCapSubsRead = 1u << 5;
    static const unsigned kTypeMidiGeneric = 1u << 1, kTypeApplication = 1u << 20;
    static const uint8_t kNoteOn = 6, kNoteOff = 7, kQueueDirect = 253, kAddrSubscribers = 254, kAddrUnknown = 253;

    void* lib_ = nullptr;
    void* seq_ = nullptr;
    SeqOpen seqOpen_ = nullptr;
    SeqClose seqClose_ = nullptr;
    SetName setName_ = nullptr;
    MkPort mkPort_ = nullptr;
    ClientId clientId_ = nullptr;
    OutDirect outDirect_ = nullptr;
    int port_ = -1, client_ = -1;
    std::atomic<bool> ok_{false};
    std::string status_ = "not open";
    bool held_[16][128] = {};
    long sent_ = 0;
};

} // namespace genramidi
