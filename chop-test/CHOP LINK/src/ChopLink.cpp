// CHOP LINK - the second half of the mini test: a plugin on another track that follows CHOP TEST.
//
// SPDX-License-Identifier: GPL-3.0-only
//
// It reads the shared memory CHOP TEST writes (common/chop_link.h) and plays a simple test tone:
//   FOLLOW = Chop:  every pad hit on CHOP TEST's track plays the root of that chop's chord (a bass note).
//   FOLLOW = Noten: the notes of this track are pulled into CHOP TEST's key (natural minor / major).
// STATUS and INFO show whether the link works and what CHOP TEST is doing.
//
// Signal flow: note -> one bass voice: saw (PolyBLEP) + sine one octave down -> low-pass (SVF) that opens on the
//              hit and closes with the envelope -> soft saturation -> amp envelope (short attack, decay) -> VOLUME.
// No worker thread: the link is opened in prepare(); the texts are built on the UI thread from atomics.

#include "mpc_plugin.h"
#include "dsp/oscillator.h"
#include "dsp/svf.h"
#include "../../common/chop_link.h"

#include <chrono>
#include <cmath>

using namespace mpc;

namespace {

enum { P_VOLUME = 0, P_FOLLOW, P_OCT, P_STATUS, P_INFO, P_COUNT };

const char* const kNotes[12] = {"C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};
const char* const kQual[8] = {"", "m", "7", "maj7", "m7", "m9", "dim", "sus4"}; // hh::Quality order
const int kMinor[7] = {0, 2, 3, 5, 7, 8, 10};
const int kMajor[7] = {0, 2, 4, 5, 7, 9, 11};

} // namespace

class ChopLink : public Plugin
{
public:
    ChopLink() : Plugin(makeInfo())
    {
        addParam(floatParam("volume", "VOLUME", -40.f, 6.f, -6.f, "dB", 1));
        addParam(choiceParam("follow", "FOLLOW", {"Chop", "Noten"}, 0));
        addParam(intParam("oct", "OCT", -3, 2, -1));
        Param s = floatParam("status", "STATUS", 0.f, 1.f, 0.f); s.automatable = false; addParam(s);
        Param i = floatParam("info", "INFO", 0.f, 1.f, 0.f); i.automatable = false; addParam(i);
    }

    static PluginInfo makeInfo()
    {
        PluginInfo i;
        i.name = "CHOP LINK";
        i.vendor = "Genra";
        i.uniqueId = MPC_FOURCC('G', 'n', 'C', 'l');
        i.version = 3000;
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
        filt_.setSampleRate(sampleRate);
        if (!opened_) { opened_ = true; link_.open(); }
    }

    void reset() override { env_ = 0.f; fenv_ = 0.f; gainSmooth_ = -1.f; filt_.reset(); }

    void process(ProcessContext& ctx) override
    {
        choplink::LinkData* L = link_.data();
        if (L) L->readerBeat.fetch_add(1, std::memory_order_relaxed);
        const bool followChop = getInt(P_FOLLOW) == 0;
        const int oct = getInt(P_OCT);

        // FOLLOW = Chop: a new pad hit on CHOP TEST's track (at most one block late)
        if (L && followChop)
        {
            uint32_t t = L->trigCount.load(std::memory_order_acquire);
            if (t != lastTrig_)
            {
                lastTrig_ = t;
                int pad = L->lastPad.load(std::memory_order_relaxed);
                if (pad >= 0 && pad < choplink::kPads)
                {
                    int root = L->chordRoot[pad].load(std::memory_order_relaxed);
                    if (root >= 0) noteOn(48 + 12 * oct + root, 0.9f);
                }
            }
        }
        else if (L)
            lastTrig_ = L->trigCount.load(std::memory_order_relaxed);

        float target = std::pow(10.f, get(P_VOLUME) / 20.f);
        if (gainSmooth_ < 0.f) gainSmooth_ = target;
        float* outL = ctx.outputs[0];
        float* outR = ctx.numOutputs > 1 ? ctx.outputs[1] : nullptr;
        const float decay = std::exp(-1.f / (float)(sr_ * (followChop ? 0.45 : 0.9)));
        const float fdecay = std::exp(-1.f / (float)(sr_ * 0.12));
        int ev = 0;
        for (int i = 0; i < ctx.numFrames; ++i)
        {
            while (ev < ctx.numMidiEvents && ctx.midi[ev].sampleOffset <= i)
            {
                const MidiEvent& e = ctx.midi[ev++];
                if (followChop) continue;
                if (e.isNoteOn()) noteOn(snap(e.note(), L) + 12 * (oct + 1), e.velocity() / 127.f), heldNote_ = e.note();
                else if (e.isNoteOff() && e.note() == heldNote_) released_ = true;
            }
            gainSmooth_ += (target - gainSmooth_) * 0.002f;
            if (attack_ < 1.f) attack_ = std::fmin(1.f, attack_ + 0.004f);
            env_ *= released_ ? decay * decay : decay;
            fenv_ *= fdecay;
            if ((ctrl_++ & 15) == 0) filt_.set(90.f + 2600.f * fenv_ * vel_ + 300.f * env_, 0.35f); // every 16 samples
            float raw = 0.55f * saw_.process() + 0.8f * sub_.process();
            float y = filt_.process(raw, mpc::dsp::SVF::Mode::Lowpass);
            y = y * 1.6f / (1.f + std::fabs(y * 1.6f));  // warm saturation
            y *= env_ * attack_ * vel_ * gainSmooth_;
            outL[i] = y;
            if (outR) outR[i] = y;
        }
    }

    bool formatValue(int index, float, std::string& out) override
    {
        if (index != P_STATUS && index != P_INFO) return false;
        choplink::LinkData* L = link_.data();
        if (!L) { out = index == P_STATUS ? std::string("Link fehlt") : link_.error(); return true; }
        auto now = std::chrono::steady_clock::now();
        uint32_t wb = L->writerBeat.load();
        if (wb != lastWriterBeat_) { lastWriterBeat_ = wb; writerSeen_ = now; writerEver_ = true; }
        bool alive = writerEver_ && now - writerSeen_ < std::chrono::milliseconds(1500);
        if (index == P_STATUS) { out = alive ? "Link OK" : "kein CHOP TEST"; return true; }

        char buf[64];
        int kr = L->keyRoot.load(), pad = L->lastPad.load();
        if (kr < 0 || kr > 11) { out = "noch keine Tonart"; return true; }
        std::string key = std::string(kNotes[kr]) + (L->keyMinor.load() ? "m" : "");
        if (pad >= 0 && pad < choplink::kPads)
        {
            int r = L->chordRoot[pad].load(), q = L->chordQuality[pad].load();
            std::string chord = (r >= 0 && r < 12) ? std::string(kNotes[r]) + (q >= 0 && q < 8 ? kQual[q] : "") : std::string("HIT");
            std::snprintf(buf, sizeof buf, "%s, Pad %d %s", key.c_str(), pad + 1, chord.c_str());
        }
        else
            std::snprintf(buf, sizeof buf, "%s, %s", key.c_str(), link_.readName().substr(0, 20).c_str());
        out = buf;
        return true;
    }

private:
    void noteOn(int note, float vel)
    {
        float hz = (float)(440.0 * std::pow(2.0, (note - 69) / 12.0));
        saw_.setFrequency(hz);
        sub_.setFrequency(hz * 0.5f);
        fenv_ = 1.f;
        env_ = 1.f;
        attack_ = 0.f;
        vel_ = 0.3f + 0.7f * vel;
        released_ = false;
    }

    // a note into CHOP TEST's key (natural minor or major); unchanged if there is no key yet
    static int snap(int note, choplink::LinkData* L)
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

    choplink::Link link_;
    bool opened_ = false;
    double sr_ = 44100.0;
    uint32_t lastTrig_ = 0;
    mpc::dsp::PolyBlepOsc saw_;
    mpc::dsp::WavetableSine sub_;
    mpc::dsp::SVF filt_;
    unsigned ctrl_ = 0;
    float env_ = 0.f, fenv_ = 0.f, attack_ = 1.f, vel_ = 1.f, gainSmooth_ = -1.f;
    int heldNote_ = -1;
    bool released_ = false;
    // UI thread only
    uint32_t lastWriterBeat_ = 0;
    std::chrono::steady_clock::time_point writerSeen_{};
    bool writerEver_ = false;
};

MPC_PLUGIN_ENTRY(ChopLink)
