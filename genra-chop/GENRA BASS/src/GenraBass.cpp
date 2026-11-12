// GENRA BASS - the first companion of GENRA CHOP: the bass on its own MPC track.
//
// SPDX-License-Identifier: GPL-3.0-only
//
// It reads LINK v2 (common/genra_link.h), the shared memory GENRA CHOP writes:
//   FOLLOW = Loop:  plays the bass line of GENRA CHOP's loop on the MPC's transport (Play / Stop of the MPC);
//                   NEW ALL on GENRA CHOP rolls it with everything, NEW here (or NEW BASS on GENRA CHOP) only the bass.
//   FOLLOW = Chop:  every chop GENRA CHOP plays gives the root of its chord as a bass note.
//   FOLLOW = Noten: only the notes of this track (the MPC's pads, a keyboard).
// The notes of this track always play too; KEY LOCK pulls them into GENRA CHOP's key.
// It beats in its LINK slot (GENRA CHOP's BASS lane lights up) and writes its SOUND as the lane's name.
//
// Signal flow: note -> one voice (SOUND: Saw Sub = saw + sine an octave down, Round = sine + a little saw,
//              808 = sine with a pitch drop) -> low-pass (SVF, TONE, opens on the hit) -> soft saturation
//              -> amp envelope (DECAY, gate from the loop or the key) -> VOLUME.
// Threads: process() (audio); a small worker copies the loop out of the shared memory (seqlock) into one of three
// buffers and handles NEW; the texts are built on the UI thread from atomics.

#include "mpc_plugin.h"
#include "dsp/oscillator.h"
#include "dsp/svf.h"
#include "../../common/genra_link.h"

#include <chrono>
#include <cmath>
#include <thread>

using namespace mpc;

namespace {

enum { P_TONE = 0, P_DECAY, P_GLIDE, P_VOLUME, P_SOUND, P_FOLLOW, P_OCT, P_KEYLOCK, P_NEW, P_STATUS, P_INFO, P_COUNT };

const char* const kNotes[12] = {"C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};
const char* const kQual[8] = {"", "m", "7", "maj7", "m7", "m9", "dim", "sus4"}; // hh::Quality order
const char* const kSounds[3] = {"Saw Sub", "Round", "808"};
const int kMinor[7] = {0, 2, 3, 5, 7, 8, 10};
const int kMajor[7] = {0, 2, 4, 5, 7, 9, 11};
const int kSteps = genralink::kSteps;

} // namespace

class GenraBass : public Plugin
{
public:
    GenraBass() : Plugin(makeInfo())
    {
        addParam(floatParam("tone", "TONE", 0.f, 100.f, 45.f, "%", 0));
        addParam(floatParam("decay", "DECAY", 0.f, 100.f, 50.f, "%", 0));
        addParam(floatParam("glide", "GLIDE", 0.f, 100.f, 30.f, "%", 0));
        addParam(floatParam("volume", "VOLUME", -40.f, 6.f, -6.f, "dB", 1));
        addParam(choiceParam("sound", "SOUND", {kSounds[0], kSounds[1], kSounds[2]}, 0));
        addParam(choiceParam("follow", "FOLLOW", {"Loop", "Chop", "Noten"}, 0));
        addParam(intParam("oct", "OCT", -2, 2, 0));
        addParam(boolParam("keylock", "KEY LOCK", true));
        addParam(boolParam("new", "NEW", false));
        Param s = floatParam("status", "STATUS", 0.f, 1.f, 0.f); s.automatable = false; addParam(s);
        Param i = floatParam("info", "INFO", 0.f, 1.f, 0.f); i.automatable = false; addParam(i);
        ready_.store(-1);
    }

    ~GenraBass() override
    {
        quit_.store(true);
        if (worker_.joinable()) worker_.join();
    }

    static PluginInfo makeInfo()
    {
        PluginInfo i;
        i.name = "GENRA BASS";
        i.vendor = "Genra";
        i.uniqueId = MPC_FOURCC('G', 'n', 'B', 's');
        i.version = 100;
        i.isInstrument = true;
        i.numInputs = 0;
        i.numOutputs = 2;
        return i;
    }

    void prepare(double sampleRate, int) override
    {
        sr_ = sampleRate;
        saw_.setSampleRate(sampleRate);
        saw_.setWave(mpc::dsp::Wave::Saw);
        sub_.setSampleRate(sampleRate);
        sine_.setSampleRate(sampleRate);
        filt_.setSampleRate(sampleRate);
        if (!started_)
        {
            started_ = true;
            link_.open();
            newSeen_ = newPrev_ = getInt(P_NEW);
            worker_ = std::thread([this] { workerMain(); });
        }
    }

    void reset() override
    {
        env_ = 0.f; fenv_ = 0.f; gainSmooth_ = -1.f; filt_.reset();
        gate_ = -1.0; wasPlaying_ = false; nPending_ = 0;
    }

    void process(ProcessContext& ctx) override
    {
        genralink::LinkData* L = link_.data();
        if (L) L->lane[genralink::LANE_BASS].beat.fetch_add(1, std::memory_order_relaxed);
        const int follow = getInt(P_FOLLOW);
        const int oct = getInt(P_OCT);
        const bool keyLock = getBool(P_KEYLOCK);
        sound_ = getInt(P_SOUND);

        // the newest loop the worker copied
        int r = ready_.load(std::memory_order_acquire);
        if (r >= 0) inUse_.store(r, std::memory_order_release);
        const genralink::Loop* loop = r >= 0 ? &bufs_[r] : nullptr;

        // FOLLOW = Chop: a chop GENRA CHOP played (at most one block late)
        if (L)
        {
            uint32_t t = L->trigCount.load(std::memory_order_acquire);
            if (t != lastTrig_)
            {
                lastTrig_ = t;
                int c = L->lastChop.load(std::memory_order_relaxed);
                if (follow == 1 && c >= 0 && c < genralink::kChops)
                {
                    int root = L->chordRoot[c].load(std::memory_order_relaxed);
                    if (root >= 0) noteOn(36 + 12 * oct + root, 0.9f, false, sr_ * 0.3);
                }
            }
        }

        // FOLLOW = Loop: the bass line on the MPC's transport
        const Transport& tr = ctx.transport;
        const bool playing = tr.valid && tr.playing && tr.ppqValid;
        const double bpm = tr.tempoValid && tr.bpm > 1.0 ? tr.bpm : 120.0;
        const double spStep = sr_ * 60.0 / bpm / 4.0;
        if (playing && follow == 0 && loop)
        {
            const double q0 = tr.ppqPos * 4.0, q1 = q0 + ctx.numFrames / spStep;
            if (!wasPlaying_ || nextStep_ < q0 - 1.0 || nextStep_ > q1 + 1.0) nextStep_ = std::ceil(q0 - 1e-6);
            const float swing = loop->swingX1000 / 1000.f;
            while (nextStep_ < q1)
            {
                long n = (long)std::llround(nextStep_);
                int s = (int)(((n % kSteps) + kSteps) % kSteps);
                const genralink::StepNote& sn = loop->bass[s];
                if (sn.pitch >= 0 && nPending_ < 8)
                {
                    double at = (nextStep_ - q0) * spStep;
                    if (s & 1) at += (swing - 0.5f) * 2.f * spStep;
                    Pending& p = pending_[nPending_++];
                    p.at = at;
                    p.note = clampNote(sn.pitch + 12 * oct);
                    p.vel = sn.vel / 127.f * ((sn.flags & 1) ? 1.f : 0.85f);
                    p.slide = (sn.flags & 2) != 0;
                    p.gate = std::max(0.2, sn.len16 / 16.0) * spStep;
                }
                curStep_.store(s, std::memory_order_relaxed);
                nextStep_ += 1.0;
            }
            wasPlaying_ = true;
        }
        else
        {
            if (wasPlaying_ && gate_ >= 0.0) gate_ = 0.0; // stop: the loop's note ends
            wasPlaying_ = false;
            nPending_ = 0;
            curStep_.store(-1, std::memory_order_relaxed);
        }

        float target = std::pow(10.f, get(P_VOLUME) / 20.f);
        if (gainSmooth_ < 0.f) gainSmooth_ = target;
        const float tone = get(P_TONE) / 100.f;
        const float dsec = 0.08f + 1.9f * get(P_DECAY) / 100.f;
        const float decay = std::exp(-1.f / (float)(sr_ * dsec));
        const float relc = std::exp(-1.f / (float)(sr_ * 0.03));
        const float fdecay = std::exp(-1.f / (float)(sr_ * (0.06 + 0.2 * tone)));
        const float glideTime = 0.005f + 0.25f * get(P_GLIDE) / 100.f;
        const float glideC = 1.f - std::exp(-1.f / (float)(sr_ * glideTime));
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
                    int note = keyLock ? snap(e.note(), L) : e.note();
                    noteOn(clampNote(note + 12 * oct), e.velocity() / 127.f, false, -1.0);
                    heldNote_ = e.note();
                }
                else if (e.isNoteOff() && e.note() == heldNote_) gate_ = 0.0;
                else if (e.isAllNotesOff()) gate_ = 0.0;
            }
            for (int k = 0; k < nPending_;)
            {
                if (pending_[k].at <= (double)i)
                {
                    const Pending& p = pending_[k];
                    noteOn(p.note, p.vel, p.slide, p.gate);
                    pending_[k] = pending_[--nPending_];
                }
                else ++k;
            }
            gainSmooth_ += (target - gainSmooth_) * 0.002f;
            if (attack_ < 1.f) attack_ = std::fmin(1.f, attack_ + 0.004f);
            if (gate_ > 0.0) { gate_ -= 1.0; if (gate_ < 0.0) gate_ = 0.0; }
            const bool held = gate_ < 0.0 || gate_ > 0.0;
            env_ *= held ? decay : relc;
            fenv_ *= fdecay;
            hz_ += (targetHz_ - hz_) * glideC;
            if ((ctrl_++ & 15) == 0) // every 16 samples
            {
                float f = hz_;
                if (sound_ == 2) f *= 1.f + 1.2f * drop_; // 808: the pitch falls into the note
                drop_ *= 0.96f;
                saw_.setFrequency(f);
                sub_.setFrequency(f * 0.5f);
                sine_.setFrequency(f);
                float cut = 70.f + 900.f * tone * tone + 3200.f * tone * fenv_ * vel_ + 200.f * env_;
                filt_.set(std::fmin(cut, 12000.f), 0.3f);
            }
            float raw;
            if (sound_ == 0) raw = 0.55f * saw_.process() + 0.8f * sub_.process();
            else if (sound_ == 1) { raw = 0.95f * sine_.process() + 0.18f * saw_.process(); sub_.process(); }
            else { raw = 1.1f * sine_.process(); saw_.process(); sub_.process(); }
            float y = filt_.process(raw, mpc::dsp::SVF::Mode::Lowpass);
            float drive = sound_ == 2 ? 2.4f : 1.6f;
            y = y * drive / (1.f + std::fabs(y * drive));
            y *= env_ * attack_ * vel_ * gainSmooth_;
            outL[i] = y;
            if (outR) outR[i] = y;
        }
        for (int k = 0; k < nPending_; ++k) pending_[k].at -= ctx.numFrames;
        inUse_.store(-1, std::memory_order_release);
    }

    bool formatValue(int index, float plain, std::string& out) override
    {
        if (index == P_TONE || index == P_DECAY || index == P_GLIDE)
        {
            char b[16];
            std::snprintf(b, sizeof b, "%.0f %%", plain);
            out = b;
            return true;
        }
        if (index != P_STATUS && index != P_INFO) return false;
        genralink::LinkData* L = link_.data();
        if (!L) { out = index == P_STATUS ? std::string("Link fehlt") : link_.error(); return true; }
        auto now = std::chrono::steady_clock::now();
        uint32_t wb = L->writerBeat.load();
        if (wb != lastWriterBeat_) { lastWriterBeat_ = wb; writerSeen_ = now; writerEver_ = true; }
        bool alive = writerEver_ && now - writerSeen_ < std::chrono::milliseconds(1500);
        if (index == P_STATUS)
        {
            out = !alive ? "kein GENRA CHOP" : (ready_.load() >= 0 ? "Link OK, Loop da" : "Link OK, kein Loop");
            return true;
        }
        int kr = L->keyRoot.load();
        if (kr < 0 || kr > 11) { out = "noch keine Tonart"; return true; }
        std::string key = std::string(kNotes[kr]) + (L->keyMinor.load() ? "m" : "");
        int s = curStep_.load();
        char buf[64];
        if (s >= 0)
        {
            int bar = s / 16, r = L->loop.barRoot[bar], q = L->loop.barQuality[bar];
            std::string chord = (r >= 0 && r < 12) ? std::string(kNotes[r]) + (q >= 0 && q < 8 ? kQual[q] : "") : std::string("-");
            std::snprintf(buf, sizeof buf, "%s, Takt %d: %s", key.c_str(), bar + 1, chord.c_str());
        }
        else
            std::snprintf(buf, sizeof buf, "Tonart %s, MPC gestoppt", key.c_str());
        out = buf;
        return true;
    }

private:
    struct Pending { double at; int note; float vel; bool slide; double gate; };

    static int clampNote(int n) { return n < 12 ? 12 : (n > 96 ? 96 : n); }

    // gate: samples the note is held (loop, Chop), -1: until note-off (keys)
    void noteOn(int note, float vel, bool slide, double gate)
    {
        float hz = (float)(440.0 * std::pow(2.0, (note - 69) / 12.0));
        targetHz_ = hz;
        const bool legato = slide && env_ > 0.05f;
        if (!legato)
        {
            hz_ = hz;
            fenv_ = 1.f;
            env_ = 1.f;
            attack_ = 0.f;
            drop_ = 1.f;
            vel_ = 0.3f + 0.7f * vel;
        }
        gate_ = gate;
    }

    // a note into GENRA CHOP's key (natural minor or major); unchanged if there is no key yet
    static int snap(int note, genralink::LinkData* L)
    {
        if (!L) return note;
        int root = L->keyRoot.load(std::memory_order_relaxed);
        if (root < 0 || root > 11) return note;
        const int* sc = L->keyMinor.load(std::memory_order_relaxed) ? kMinor : kMajor;
        int rel = ((note - root) % 12 + 12) % 12;
        int best = 0, bestD = 99;
        for (int k = 0; k < 7; ++k) { int d = std::abs(sc[k] - rel); if (d < bestD) { bestD = d; best = sc[k]; } }
        return note - rel + best;
    }

    void workerMain()
    {
        uint32_t lastSeq = 1; // odd: never a finished loop, so the first one is taken
        int lastSound = -1;
        genralink::Loop tmp;
        while (!quit_.load())
        {
            genralink::LinkData* L = link_.data();
            if (L)
            {
                uint32_t seq = 0;
                if (L->magic.load() == genralink::kMagic && L->loopSeq.load() != lastSeq && link_.readLoop(tmp, seq) && seq != 0)
                {
                    lastSeq = seq;
                    int r = ready_.load(), u = inUse_.load(), w = 0;
                    while (w == r || w == u) ++w;
                    bufs_[w] = tmp;
                    ready_.store(w, std::memory_order_release);
                }
                int snd = getInt(P_SOUND);
                if (snd != lastSound) { lastSound = snd; genralink::Link::writeName(L->lane[genralink::LANE_BASS], kSounds[snd]); }
                // NEW: a tap flips the parameter; a change that stays (>= 25 ms) is a tap, not the MPC probing
                int v = getInt(P_NEW);
                if (v != newSeen_ && v == newPrev_)
                {
                    newSeen_ = v;
                    L->lane[genralink::LANE_BASS].newRequest.fetch_add(1, std::memory_order_acq_rel);
                }
                newPrev_ = v;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
    }

    genralink::Link link_;
    bool started_ = false;
    std::thread worker_;
    std::atomic<bool> quit_{false};
    genralink::Loop bufs_[3];
    std::atomic<int> ready_{-1}, inUse_{-1}, curStep_{-1};
    int newSeen_ = 0, newPrev_ = 0;
    double sr_ = 44100.0;
    uint32_t lastTrig_ = 0;
    mpc::dsp::PolyBlepOsc saw_;
    mpc::dsp::WavetableSine sub_, sine_;
    mpc::dsp::SVF filt_;
    unsigned ctrl_ = 0;
    int sound_ = 0;
    float env_ = 0.f, fenv_ = 0.f, attack_ = 1.f, vel_ = 1.f, gainSmooth_ = -1.f, drop_ = 0.f;
    float hz_ = 55.f, targetHz_ = 55.f;
    double gate_ = -1.0, nextStep_ = 0.0;
    bool wasPlaying_ = false;
    Pending pending_[8];
    int nPending_ = 0;
    int heldNote_ = -1;
    // UI thread only
    uint32_t lastWriterBeat_ = 0;
    std::chrono::steady_clock::time_point writerSeen_{};
    bool writerEver_ = false;
};

MPC_PLUGIN_ENTRY(GenraBass)
