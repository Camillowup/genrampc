// SCHLAG - the drum companion of GENRA CHOP: eight sampled voices that follow GENRA CHOP's drums, with the
// random sample of the Polyend Play: a voice can swap its sample on some of its hits for others from its folder.
//
// SPDX-License-Identifier: GPL-3.0-only
//
// Pages (genra-chop/tools/build_skin.py), on the function keys:
//   SCHLAG  eight columns KICK, SNARE, CLAP, RIM, HAT ZU, HAT OFFEN, TOM, PERC: lamp, M / S, folder and sample
//           (tap: the voice for the ORDNER page), the bar's hits (red: a swapped sample), RANDOM (how many hits
//           swap; every turn rolls again; 0 % = the voice's own sample on every hit), four function knobs (tap the
//           function's name to step through TUNE, DECAY, DRIVE, BITS, REPEAT, PAN, REVERSE, FILTER), the eight pads
//           to play; header: LINK, AUTO | PADS, UNDO, BASIC (every RANDOM to 0), NEW. CLAP and TOM have displays that
//           are dying (on purpose).
//   ORDNER  the voice (8 buttons), its folder: rows to tap (folder: open; WAV: first tap = listen, second tap = the
//           voice's sample; its folder becomes the voice's folder), ZURUECK, page buttons, HAUPTORDNER HIER, LISTE
//   PRINT   the drums as a MIDI file (pad notes); afterwards PADS
//   SETUP   LERNEN (the pads, shared with every GENRA plugin), CHOKE (closed hat cuts the open hat), the status
//
// The pattern comes from GENRA CHOP over LINK v3 (common/genra_link.h): velocity, timing, ratchets, chance, every-2nd
// per voice and step. AUTO plays it on the MPC's transport; PADS only the notes of this track (pads 1-8 = voices).
// NEW / UNDO ask GENRA CHOP for new drums / the last ones.
//
// Signal flow per hit: sample (16-bit in memory, up to 24 per folder, 2 s each) -> TUNE (rate) / REVERSE -> DECAY
// -> BITS -> DRIVE -> FILTER -> PAN -> LEVEL, velocity -> sum -> VOLUME -> a fixed peak limiter -> out 1/2.
// Hall and delay: the MPC track's sends. Threads: process() (audio) only plays and touches atomics; a worker loads
// folders, builds the screen and writes files.

#include "mpc_plugin.h"
#include "../../common/genra_files.h"
#include "../../common/genra_link.h"
#include "../../common/genra_ui.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <mutex>
#include <thread>

using namespace mpc;
using namespace genrafiles;

namespace {

static const int kVoices = 8, kSlots = 4, kSteps = genralink::kSteps, kRows = 8, kPrintRows = 7;
static const int kMaxFiles = 24, kFlashes = 12;
// MIDI channel 10 (GENRA CHOP OUT): the notes of pads A01-A08 of a new MPC drum program, voice order KICK .. PERC
const int kKitNote[8] = {37, 36, 42, 82, 40, 38, 46, 44};
static const float kMaxSeconds = 2.f;
const char* const kVoiceNames[kVoices] = {"KICK", "SNARE", "CLAP", "RIM", "CL HAT", "OP HAT", "TOM", "PERC"};
enum Func { F_TUNE, F_DECAY, F_DRIVE, F_BITS, F_REPEAT, F_PAN, F_REVERSE, F_FILTER, NUM_FUNCS };
const char* const kFuncNames[NUM_FUNCS] = {"TUNE", "DECAY", "DRIVE", "BITS", "REPEAT", "PAN", "REVERSE", "FILTER"};
const int kDefaultFunc[kSlots] = {F_TUNE, F_DECAY, F_DRIVE, F_PAN};

static const char* const kCaptions[][2] = {
    {"schlag", "SCHLAG"}, {"auto", "AUTO"}, {"padsmode", "PADS"}, {"undo", "UNDO"}, {"basic", "BASIC"}, {"new", "NEW"},
    {"m", "M"}, {"s", "S"}, {"v1", "KICK"}, {"v2", "SNARE"}, {"v3", "CLAP"}, {"v4", "RIM"}, {"v5", "CL HAT"},
    {"v6", "OP HAT"}, {"v7", "TOM"}, {"v8", "PERC"}, {"p1", "1"}, {"p2", "2"}, {"p3", "3"}, {"p4", "4"},
    {"p5", "5"}, {"p6", "6"}, {"p7", "7"}, {"p8", "8"}, {"ordner", "· FOLDER"}, {"back", "BACK"},
    {"drives", "DRIVES"}, {"pgup", "< PAGE"}, {"pgdn", "PAGE >"}, {"root", "ROOT HERE"},
    {"hint1", "1st tap = listen"}, {"hint2", "2nd tap = the voice's sample"}, {"print", "PRINT"},
    {"printtitle", "· PRINT"}, {"after", "After PRINT"}, {"autoinfo", "AUTO: the loop plays"},
    {"padsinfo", "PADS: only the MPC track's notes"}, {"learn", "LEARN"}, {"help1", "Tap LEARN, then press"},
    {"help2", "pads 1 to 16 in order"}, {"help3", "(pad 1 = bottom left)"},
    {"help4", "Used by every GENRA plugin"}, {"choke", "HAT CHOKE"}, {"r1", "Pad map"}, {"r2", "Root folder"},
    {"r3", "LINK"}, {"r4", "Memory"}, {"r5", "Version"}, {"setup", "· SETUP"}, {"savesmp", "SAVE SAMPLE"},
    {"savekit", "SAVE KIT"}, {"kitnotes", "MPC KIT NOTES"}, {"kithelp1", "MIDI ch 10: MPC kit notes"},
    {"kithelp2", "(A01-A08 = KICK .. PERC)"}, {"kithelp3", "ON: on every channel"}};
static const int kNumCaps = (int)(sizeof(kCaptions) / sizeof(kCaptions[0]));

enum
{
    // per voice v: RANDOM (Q-Links 1-8), LEVEL (Q-Links 9-16), then the rest
    P_RANDOM0 = 0,
    P_LEVEL0 = P_RANDOM0 + kVoices,
    P_MUTE0 = P_LEVEL0 + kVoices,
    P_SOLO0 = P_MUTE0 + kVoices,
    P_FUNC0 = P_SOLO0 + kVoices,          // v * 4 + k
    P_VAL0 = P_FUNC0 + kVoices * kSlots,  // v * 4 + k
    P_VOLUME = P_VAL0 + kVoices * kSlots, P_AUTO, P_CHOKE, P_VOICE, P_KITNOTES,
    // buttons
    P_NEW, P_UNDO, P_BASIC, P_PRINT, P_LEARN, P_BACK, P_DRVBTN, P_PGUP, P_PGDN, P_ROOTHERE, P_SAVESMP, P_SAVEKIT, // flashes 0..11
    P_SCROLL,
    P_FUNCTAP0,                            // v * 4 + k: the function's name, tap = next function
    P_PICK0 = P_FUNCTAP0 + kVoices * kSlots, // folder / sample fields: the voice for ORDNER
    P_PADTAP0 = P_PICK0 + kVoices,
    P_ROWTAP0 = P_PADTAP0 + kVoices,
    // texts
    P_HDRLINK = P_ROWTAP0 + kRows, P_PATH, P_LISTINFO, P_VOICEINFO, P_VFOLDER, P_VSAMPLE, P_VCOUNT, P_STATUS,
    P_PRINTINFO, P_PRINTSTATUS, P_PRINTDIRTXT, P_LEARNINFO, P_MAPINFO, P_ROOTTXT, P_LINKINFO, P_MEMINFO, P_VERSION,
    P_FOLDERTXT0,
    P_SAMPLETXT0 = P_FOLDERTXT0 + kVoices,
    P_ROWT0 = P_SAMPLETXT0 + kVoices,
    P_PRINTROW0 = P_ROWT0 + kRows,
    P_CAP0 = P_PRINTROW0 + kPrintRows,
    // film strips
    P_CELL0 = P_CAP0 + kNumCaps,          // v * 16 + j: 0 none, 1 hit, 2 hit with a swapped sample
    P_LAMP0 = P_CELL0 + kVoices * 16,
    P_PADLIT0 = P_LAMP0 + kVoices,
    P_FLASH0 = P_PADLIT0 + kVoices,
    P_COUNT = P_FLASH0 + kFlashes
};

// a sample in memory (16 bit)
struct Sample
{
    std::vector<int16_t> L, R; // R empty: mono
    float rate = 44100.f;
    std::string name;
    size_t frames() const { return L.size(); }
};

// what a voice can play: its folder's samples, the base one
struct Kit
{
    std::vector<Sample> samples;
    int base = 0;
};

struct Player
{
    const Sample* s = nullptr;
    bool active = false, rev = false;
    double pos = 0.0, rate = 1.0, end = 0.0;
    float gain = 1.f, panL = 1.f, panR = 1.f, drive = 0.f, decayLen = 0.f;
    int bits = 16;
    float fa = 1.f, zl = 0.f, zr = 0.f;
    int age = 0, fade = -1;
};

struct Hit { double at; int voice; float vel; int step; uint32_t round; };

Sample synth(int voice, float sr)
{
    Sample s;
    s.rate = sr;
    s.name = std::string(kVoiceNames[voice]) + " (built in)";
    const float lens[kVoices] = {0.45f, 0.25f, 0.3f, 0.08f, 0.06f, 0.4f, 0.4f, 0.12f};
    size_t n = (size_t)(lens[voice] * sr);
    s.L.resize(n);
    uint32_t rng = 1234u + (uint32_t)voice * 777u;
    auto noise = [&rng]() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return (rng >> 8) * (1.f / 8388608.f) - 1.f; };
    float lp = 0.f, hp = 0.f, ph = 0.f;
    for (size_t i = 0; i < n; ++i)
    {
        float t = i / sr, v = 0.f;
        switch (voice)
        {
            case 0: ph += 2.f * 3.14159f * (45.f + 110.f * std::exp(-t * 35.f)) / sr; v = std::sin(ph) * std::exp(-t * 7.f); break;
            case 1: ph += 2.f * 3.14159f * 190.f / sr; v = (0.5f * std::sin(ph) * std::exp(-t * 25.f) + 0.6f * noise() * std::exp(-t * 14.f)); break;
            case 2: { float e = std::exp(-std::fmod(t, 0.012f) * 300.f) * (t < 0.036f ? 1.f : 0.f) + std::exp(-t * 18.f) * (t >= 0.024f ? 1.f : 0.f); lp += 0.35f * (noise() - lp); v = (noise() - lp) * e * 0.8f; break; }
            case 3: ph += 2.f * 3.14159f * 820.f / sr; v = std::sin(ph) * std::exp(-t * 60.f) * 0.7f; break;
            case 4: { float x = noise(); hp = x - lp; lp += 0.6f * (x - lp); v = hp * std::exp(-t * 70.f) * 0.6f; break; }
            case 5: { float x = noise(); hp = x - lp; lp += 0.6f * (x - lp); v = hp * std::exp(-t * 7.f) * 0.5f; break; }
            case 6: ph += 2.f * 3.14159f * (95.f + 40.f * std::exp(-t * 20.f)) / sr; v = std::sin(ph) * std::exp(-t * 8.f) * 0.8f; break;
            default: { float x = noise(); hp = x - lp; lp += 0.3f * (x - lp); v = hp * std::exp(-t * 30.f) * 0.5f; break; }
        }
        s.L[i] = (int16_t)std::lround(std::max(-1.f, std::min(1.f, v)) * 32000.f);
    }
    return s;
}

} // namespace

class Schlag : public Plugin
{
public:
    Schlag() : Plugin(makeInfo()), screen_(*this)
    {
        for (int v = 0; v < kVoices; ++v) addParam(floatParam(("random" + std::to_string(v + 1)).c_str(), ("RANDOM " + std::string(kVoiceNames[v])).c_str(), 0.f, 100.f, 0.f, "%", 0));
        for (int v = 0; v < kVoices; ++v) addParam(floatParam(("level" + std::to_string(v + 1)).c_str(), ("LEVEL " + std::string(kVoiceNames[v])).c_str(), -40.f, 6.f, 0.f, "dB", 1));
        for (int v = 0; v < kVoices; ++v) addParam(boolParam(("mute" + std::to_string(v + 1)).c_str(), ("M " + std::to_string(v + 1)).c_str(), false));
        for (int v = 0; v < kVoices; ++v) addParam(boolParam(("solo" + std::to_string(v + 1)).c_str(), ("S " + std::to_string(v + 1)).c_str(), false));
        std::vector<std::string> funcs(kFuncNames, kFuncNames + NUM_FUNCS);
        for (int v = 0; v < kVoices; ++v)
            for (int k = 0; k < kSlots; ++k)
                addParam(choiceParam(("func" + std::to_string(v + 1) + "_" + std::to_string(k + 1)).c_str(), ("FUNCTION " + std::to_string(v + 1) + " " + std::to_string(k + 1)).c_str(), funcs, kDefaultFunc[k]));
        for (int v = 0; v < kVoices; ++v)
            for (int k = 0; k < kSlots; ++k)
                addParam(floatParam(("val" + std::to_string(v + 1) + "_" + std::to_string(k + 1)).c_str(), ("VALUE " + std::to_string(v + 1) + " " + std::to_string(k + 1)).c_str(), 0.f, 100.f, defaultValue(kDefaultFunc[k]), "", 0));
        addParam(floatParam("volume", "VOLUME", -40.f, 6.f, 0.f, "dB", 1));
        addParam(choiceParam("auto", "AUTO PADS", {"Auto", "Pads"}, 0));
        addParam(boolParam("choke", "HAT CHOKE", true));
        addParam(choiceParam("voice", "VOICE", {"KICK", "SNARE", "CLAP", "RIM", "CL HAT", "OP HAT", "TOM", "PERC"}, 0));
        addParam(boolParam("kitnotes", "MPC KIT NOTES", false));
        for (const char* b : {"NEW", "UNDO", "BASIC", "PRINT", "LEARN", "FOLDER UP", "DRIVES", "PAGE BACK", "PAGE NEXT", "ROOT HERE", "SAVE SAMPLE", "SAVE KIT"})
            addButton(b);
        addParam(intParam("scroll", "LIST", 0, 1000, 500));
        for (int v = 0; v < kVoices; ++v)
            for (int k = 0; k < kSlots; ++k) addButton(("FUNKTIONSWAHL " + std::to_string(v + 1) + " " + std::to_string(k + 1)).c_str());
        for (int v = 0; v < kVoices; ++v) addButton(("WAHL " + std::to_string(v + 1)).c_str());
        for (int v = 0; v < kVoices; ++v) addButton(("PAD " + std::to_string(v + 1)).c_str());
        for (int r = 0; r < kRows; ++r) addButton(("TIPP " + std::to_string(r + 1)).c_str());
        for (const char* t : {"KOPFLINK", "PFAD", "LISTINFO", "STIMMINFO", "STIMMORDNER", "STIMMSAMPLE", "STIMMANZAHL",
                              "STATUS", "PRINTINFO", "PRINTSTATUS", "PRINTORDNER", "LERNINFO", "BELEGUNG", "HAUPTORDNER",
                              "LINKINFO", "SPEICHERINFO", "VERSION"})
            addText(t);
        for (int v = 0; v < kVoices; ++v) addText(("ORDNERTEXT " + std::to_string(v + 1)).c_str());
        for (int v = 0; v < kVoices; ++v) addText(("SAMPLETEXT " + std::to_string(v + 1)).c_str());
        for (int r = 0; r < kRows; ++r) addText(("ZEILE " + std::to_string(r + 1)).c_str());
        for (int r = 0; r < kPrintRows; ++r) addText(("PRINTZEILE " + std::to_string(r + 1)).c_str());
        for (const auto& c : kCaptions) addText((std::string("CAP ") + c[0]).c_str());
        for (int v = 0; v < kVoices; ++v)
            for (int j = 0; j < 16; ++j) addText(("ZELLE " + std::to_string(v + 1) + " " + std::to_string(j + 1)).c_str());
        for (int v = 0; v < kVoices; ++v) addText(("LAMPE " + std::to_string(v + 1)).c_str());
        for (int v = 0; v < kVoices; ++v) addText(("PADLICHT " + std::to_string(v + 1)).c_str());
        for (int i = 0; i < kFlashes; ++i) addText(("BLITZ " + std::to_string(i + 1)).c_str());
        if (numParams() != P_COUNT) std::abort();

        screen_.init();
        for (int i = P_HDRLINK; i < P_CELL0; ++i) screen_.text(i);
        for (int i = 0; i < kNumCaps; ++i) screen_.text(P_CAP0 + i, kCaptions[i][1]);
        for (int i = 0; i < kVoices * 16; ++i) screen_.strip(P_CELL0 + i, 3);
        for (int v = 0; v < kVoices; ++v) { screen_.strip(P_LAMP0 + v, 2); screen_.strip(P_PADLIT0 + v, 2); }
        for (int i = 0; i < kFlashes; ++i) screen_.button(P_NEW + i, P_FLASH0 + i);
        for (int i = P_FUNCTAP0; i < P_ROWTAP0 + kRows; ++i) if (i < P_PADTAP0 || i >= P_ROWTAP0) screen_.button(i);
        screen_.watch(P_SCROLL);
        screen_.watch(P_AUTO);
        screen_.setText(P_VERSION, "SCHLAG 0.4 · LINK v3");

        for (int v = 0; v < kVoices; ++v)
        {
            kit_[v].store(nullptr);
            pendingKit_[v].store(nullptr);
            for (auto& r : retired_[v]) r.store(nullptr);
            seed_[v].store(1u + (uint32_t)v * 97u);
            swapped_[v].store(0);
        }
        preview_.store(nullptr);
        for (auto& r : previewRetired_) r.store(nullptr);
    }

    ~Schlag() override
    {
        quit_.store(true);
        if (worker_.joinable()) worker_.join();
        for (int v = 0; v < kVoices; ++v)
        {
            delete kit_[v].exchange(nullptr);
            delete pendingKit_[v].exchange(nullptr);
            for (auto& r : retired_[v]) delete r.exchange(nullptr);
        }
        delete preview_.exchange(nullptr);
        delete previewPending_.exchange(nullptr);
        for (auto& r : previewRetired_) delete r.exchange(nullptr);
    }

    static PluginInfo makeInfo()
    {
        PluginInfo i;
        i.name = "SCHLAG";
        i.vendor = "Genra";
        i.uniqueId = MPC_FOURCC('G', 'n', 'S', 'g');
        i.version = 400;
        i.isInstrument = true;
        i.numInputs = 0;
        i.numOutputs = 2;
        return i;
    }

    void prepare(double sampleRate, int) override
    {
        sr_ = sampleRate;
        if (!started_)
        {
            started_ = true;
            link_.open();
            pads_.load();
            settings_.load();
            for (int v = 0; v < kVoices; ++v)
            {
                Kit* k = new Kit();
                k->samples.push_back(synth(v, (float)sampleRate));
                kitInfo_[v] = {std::string(), "built in", 1};
                delete pendingKit_[v].exchange(k);
            }
            worker_ = std::thread([this] { workerMain(); });
        }
    }

    void reset() override
    {
        for (auto& p : players_) for (auto& q : p) q.active = false;
        nHits_ = 0; wasPlaying_ = false; gainSmooth_ = -1.f; limGain_ = 1.f;
        for (int v = 0; v < kVoices; ++v) padSeen_[v] = getInt(P_PADTAP0 + v);
    }

    void process(ProcessContext& ctx) override
    {
        for (int v = 0; v < kVoices; ++v)
            if (Kit* k = pendingKit_[v].exchange(nullptr, std::memory_order_acq_rel))
            {
                for (auto& q : players_[v]) q.active = false;
                if (Kit* old = kit_[v].exchange(k)) retire(old, retired_[v]);
            }
        if (Sample* p = previewPending_.exchange(nullptr, std::memory_order_acq_rel))
        {
            if (Sample* old = preview_.exchange(p)) retire(old, previewRetired_);
            previewPos_ = 0.0;
        }
        genralink::LinkData* L = link_.data();
        if (L) L->lane[genralink::LANE_DRUMS].beat.fetch_add(1, std::memory_order_relaxed);
        // the loop that plays: a new one from GENRA CHOP takes over at the end of the pattern (or at once when stopped)
        const Transport& tr0 = ctx.transport;
        const bool playing0 = tr0.valid && tr0.playing && tr0.ppqValid;
        if (cur_ < 0 || !playing0) { int r = ready_.load(std::memory_order_acquire); if (r >= 0) cur_ = r; }
        if (cur_ >= 0) inUse_.store(cur_, std::memory_order_release);
        const genralink::Loop* loop = cur_ >= 0 ? &loops_[cur_] : nullptr;
        long len = L ? (long)L->loopSteps.load(std::memory_order_relaxed) : 0;
        if (len != 16 && len != 32 && len != 64 && len != 128) len = kSteps;

        for (int v = 0; v < kVoices; ++v)
        {
            int t = getInt(P_PADTAP0 + v);
            if (t != padSeen_[v]) { padSeen_[v] = t; trigger(v, 0.85f, -1, 0); }
        }

        const Transport& tr = ctx.transport;
        const bool playing = tr.valid && tr.playing && tr.ppqValid;
        const bool autoMode = getInt(P_AUTO) == 0;
        const double bpm = tr.tempoValid && tr.bpm > 1.0 ? tr.bpm : 120.0;
        const double spStep = sr_ * 60.0 / bpm / 4.0;
        if (playing && autoMode && cur_ >= 0)
        {
            const double q0 = tr.ppqPos * 4.0, q1 = q0 + ctx.numFrames / spStep;
            if (!wasPlaying_ || nextStep_ < q0 - 1.0 || nextStep_ > q1 + 1.0) nextStep_ = std::ceil(q0 - 1e-6);
            const float swing = loop->swingX1000 / 1000.f;
            while (nextStep_ < q1)
            {
                long n = (long)std::llround(nextStep_);
                long pos = ((n % len) + len) % len;
                int s = (int)(pos % kSteps);
                if (pos == 0)
                {
                    int r = ready_.load(std::memory_order_acquire);
                    if (r >= 0 && r != cur_) { cur_ = r; inUse_.store(cur_, std::memory_order_release); loop = &loops_[cur_]; }
                }
                uint32_t round = (uint32_t)(n / kSteps);
                double base = (nextStep_ - q0) * spStep + ((s & 1) ? (swing - 0.5f) * 2.f * spStep : 0.0);
                for (int v = 0; v < kVoices; ++v)
                {
                    const genralink::DrumHit& h = loop->drum[v][s];
                    if (!h.vel) continue;
                    if (h.cond == 1 && (round & 1u)) continue;
                    if (h.chance < 255 && ((uint32_t)(s * 131 + v * 17 + round * 7919) * 2654435761u >> 24) > h.chance) continue;
                    int rat = std::max(1, (int)h.ratchet);
                    int rep = repeatOf(v);
                    if (rep > 1) rat = std::max(rat, rep);
                    double at = base + (h.off > 0 ? h.off / 100.0 * spStep : 0.0);
                    for (int k = 0; k < rat && nHits_ < 64; ++k)
                        hits_[nHits_++] = {at + k * spStep / rat, v, h.vel / 127.f * (k ? 0.75f : 1.f), s, round};
                }
                if (s == 0) for (auto& sw : swapped_) sw.store(0, std::memory_order_relaxed); // a new pass: new marks
                curStep_.store(s, std::memory_order_relaxed);
                nextStep_ += 1.0;
            }
            wasPlaying_ = true;
        }
        else
        {
            wasPlaying_ = false;
            nHits_ = 0;
            curStep_.store(playing ? (int)((((long)std::floor(tr.ppqPos * 4.0) % kSteps) + kSteps) % kSteps) : -1);
        }

        // the controls, once per block
        bool solo = false;
        for (int v = 0; v < kVoices; ++v) if (getBool(P_SOLO0 + v)) solo = true;
        for (int v = 0; v < kVoices; ++v)
        {
            audible_[v] = !getBool(P_MUTE0 + v) && (!solo || getBool(P_SOLO0 + v));
            levelGain_[v] = std::pow(10.f, get(P_LEVEL0 + v) / 20.f);
        }
        float target = std::pow(10.f, get(P_VOLUME) / 20.f);
        if (gainSmooth_ < 0.f) gainSmooth_ = target;
        const float limRel = std::exp(-1.f / (float)(sr_ * 0.12));
        const float ceiling = 0.966f; // -0.3 dB
        float* outL = ctx.outputs[0];
        float* outR = ctx.numOutputs > 1 ? ctx.outputs[1] : nullptr;
        int ev = 0;
        uint32_t lit = 0;
        const bool kitNotes = getBool(P_KITNOTES);
        for (int i = 0; i < ctx.numFrames; ++i)
        {
            while (ev < ctx.numMidiEvents && ctx.midi[ev].sampleOffset <= i)
            {
                const MidiEvent& e = ctx.midi[ev++];
                if (!e.isNoteOn()) continue;
                if (pads_.learnNote(e.note())) continue;
                // channel 10 (GENRA CHOP OUT, MPC drum notes) or MPC KIT NOTES: A01-A08 = the voices; else the pad map
                int pad = (e.channel() == 9 || kitNotes) ? kitVoice(e.note()) : pads_.padOf(e.note());
                if (pad >= 0 && pad < kVoices) trigger(pad, e.velocity() / 127.f, -1, 0);
            }
            for (int k = 0; k < nHits_;)
            {
                if (hits_[k].at <= (double)i) { trigger(hits_[k].voice, hits_[k].vel, hits_[k].step, hits_[k].round); hits_[k] = hits_[--nHits_]; }
                else ++k;
            }
            float l = 0.f, rr = 0.f;
            for (int v = 0; v < kVoices; ++v)
                for (Player& p : players_[v])
                {
                    if (!p.active) continue;
                    float a, b;
                    if (!sampleAt(p, a, b)) { p.active = false; continue; }
                    float env = p.gain;
                    if (p.decayLen > 0.f) env *= std::max(0.f, 1.f - p.age / p.decayLen);
                    if (p.age < 16) env *= p.age / 16.f;
                    if (p.fade >= 0) { env *= p.fade / 64.f; if (--p.fade <= 0) { p.active = false; continue; } }
                    if (p.bits < 16) { float q = (float)(1 << (p.bits - 1)); a = std::round(a * q) / q; b = std::round(b * q) / q; }
                    if (p.drive > 0.01f) { float d = 1.f + 8.f * p.drive; a = std::tanh(a * d) / std::tanh(d); b = std::tanh(b * d) / std::tanh(d); }
                    if (p.fa < 1.f) { p.zl += p.fa * (a - p.zl); p.zr += p.fa * (b - p.zr); a = p.zl; b = p.zr; }
                    if (audible_[v]) { l += a * env * p.panL; rr += b * env * p.panR; lit |= 1u << v; }
                    p.pos += p.rev ? -p.rate : p.rate;
                    ++p.age;
                }
            if (Sample* pv = preview_.load(std::memory_order_relaxed))
            {
                size_t k = (size_t)previewPos_;
                if (k + 1 < pv->frames())
                {
                    float a = pv->L[k] / 32768.f, b = pv->R.empty() ? a : pv->R[k] / 32768.f;
                    l += a * 0.5f; rr += b * 0.5f;
                    previewPos_ += pv->rate / sr_;
                }
            }
            gainSmooth_ += (target - gainSmooth_) * 0.002f;
            l *= gainSmooth_; rr *= gainSmooth_;
            float peak = std::max(std::fabs(l), std::fabs(rr));
            float need = peak > ceiling ? ceiling / peak : 1.f;
            limGain_ = need < limGain_ ? need : 1.f - (1.f - limGain_) * limRel;
            outL[i] = l * limGain_;
            if (outR) outR[i] = rr * limGain_;
        }
        for (int k = 0; k < nHits_; ++k) hits_[k].at -= ctx.numFrames;
        litMask_.store(lit, std::memory_order_relaxed);
        // inUse_ keeps cur_: the worker must not overwrite the loop that plays
    }

    void saveExtraState(std::vector<uint8_t>& out) override
    {
        std::lock_guard<std::mutex> g(mtx_);
        std::string s = "SG1\n";
        for (int v = 0; v < kVoices; ++v) s += voiceFolder_[v] + "\t" + voiceFile_[v] + "\n";
        out.assign(s.begin(), s.end());
    }

    void loadExtraState(const uint8_t* data, size_t size) override
    {
        std::lock_guard<std::mutex> g(mtx_);
        std::string s(reinterpret_cast<const char*>(data), size);
        if (s.compare(0, 4, "SG1\n") != 0) return;
        size_t pos = 4;
        for (int v = 0; v < kVoices && pos < s.size(); ++v)
        {
            size_t e = s.find('\n', pos);
            std::string line = s.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
            pos = e == std::string::npos ? s.size() : e + 1;
            size_t t = line.find('\t');
            voiceFolder_[v] = t == std::string::npos ? line : line.substr(0, t);
            voiceFile_[v] = t == std::string::npos ? std::string() : line.substr(t + 1);
            loadReq_[v] = true;
        }
        resyncReq_.store(true);
    }

    bool formatValue(int index, float plain, std::string& out) override
    {
        char b[32];
        if (index >= P_RANDOM0 && index < P_RANDOM0 + kVoices) { std::snprintf(b, sizeof b, "%.0f %%", plain); out = b; return true; }
        if (index >= P_LEVEL0 && index < P_LEVEL0 + kVoices) { std::snprintf(b, sizeof b, "%+.1f dB", plain); out = b; return true; }
        if (index >= P_FUNC0 && index < P_FUNC0 + kVoices * kSlots) { out = std::string(kFuncNames[clamp((int)std::lround(plain), 0, NUM_FUNCS - 1)]) + " >"; return true; }
        if (index >= P_VAL0 && index < P_VAL0 + kVoices * kSlots)
        {
            int f = clamp(getInt(P_FUNC0 + (index - P_VAL0)), 0, NUM_FUNCS - 1);
            out = valueText(f, plain);
            return true;
        }
        if (index >= P_CELL0) return false;
        return screen_.format(index, out);
    }

private:
    static float defaultValue(int f)
    {
        switch (f) { case F_TUNE: case F_PAN: return 50.f; case F_DECAY: case F_BITS: case F_FILTER: return 100.f; default: return 0.f; }
    }
    static std::string valueText(int f, float v)
    {
        char b[32];
        switch (f)
        {
            case F_TUNE: std::snprintf(b, sizeof b, "%+d st", (int)std::lround((v - 50.f) * 0.24f)); break;
            case F_DECAY: std::snprintf(b, sizeof b, v >= 99.5f ? "full" : "%.0f %%", v); break;
            case F_DRIVE: std::snprintf(b, sizeof b, "%.0f %%", v); break;
            case F_BITS: std::snprintf(b, sizeof b, "%d", bitsOf(v)); break;
            case F_REPEAT: { const char* r[4] = {"off", "1/32", "1/48", "1/64"}; std::snprintf(b, sizeof b, "%s", r[repeatIndex(v)]); break; }
            case F_PAN: { int p = (int)std::lround((v - 50.f) * 2.f); if (!p) std::snprintf(b, sizeof b, "Center"); else std::snprintf(b, sizeof b, "%s %d", p < 0 ? "L" : "R", std::abs(p)); break; }
            case F_REVERSE: std::snprintf(b, sizeof b, v >= 50.f ? "on" : "off"); break;
            default: if (v >= 99.5f) std::snprintf(b, sizeof b, "open"); else std::snprintf(b, sizeof b, "LP %.1fk", cutoffHz(v) / 1000.f); break;
        }
        return b;
    }
    static int kitVoice(int note)
    {
        for (int v = 0; v < kVoices; ++v) if (kKitNote[v] == note) return v;
        return -1;
    }
    static int bitsOf(float v) { return clamp((int)std::lround(4.f + v * 0.12f), 4, 16); }
    static int repeatIndex(float v) { return clamp((int)(v / 25.f), 0, 3); }
    static float cutoffHz(float v) { return 150.f * std::pow(2.f, v / 100.f * 7.f); }

    void addText(const char* name)
    {
        std::string id = name;
        for (char& c : id) c = c == ' ' ? '_' : (char)::tolower((unsigned char)c);
        Param p = floatParam(id.c_str(), name, 0.f, 1.f, 0.f);
        p.automatable = false;
        addParam(p);
    }
    void addButton(const char* name)
    {
        std::string id = name;
        for (char& c : id) c = c == ' ' ? '_' : (char)::tolower((unsigned char)c);
        addParam(boolParam(id.c_str(), name, false));
    }

    template <typename T, size_t N>
    static void retire(T* s, std::atomic<T*> (&bin)[N])
    {
        for (auto& r : bin)
        {
            T* expected = nullptr;
            if (r.compare_exchange_strong(expected, s)) return;
        }
    }

    // ------------------------------------------------------------------ audio
    float funcValue(int v, int f, float def) const
    {
        float out = def;
        for (int k = 0; k < kSlots; ++k) if (getInt(P_FUNC0 + v * kSlots + k) == f) out = get(P_VAL0 + v * kSlots + k);
        return out;
    }
    // REPEAT: hits per step (aus, 1/32 = 2, 1/48 = 3, 1/64 = 4)
    int repeatOf(int v) const { return repeatIndex(funcValue(v, F_REPEAT, 0.f)) + 1; }

    // the sample a hit plays: the base one, or (RANDOM) another one from the folder
    int pickSample(int v, const Kit& k, int step, uint32_t round) const
    {
        int n = (int)k.samples.size();
        if (n <= 1 || step < 0) return k.base;
        float pct = get(P_RANDOM0 + v) / 100.f;
        if (pct <= 0.f) return k.base;
        uint32_t seed = seed_[v].load(std::memory_order_relaxed);
        // a new roll on every pass of the loop (round), like the random sample of the Polyend Play
        uint32_t h = (uint32_t)step * 2654435761u ^ seed * 40503u ^ (uint32_t)v * 9176u ^ round * 0x9e3779b9u;
        h ^= h >> 15; h *= 0x2c1b3c6du; h ^= h >> 12; h *= 0x297a2d39u; h ^= h >> 15;
        if ((h & 0xffff) / 65535.f >= pct) return k.base;
        int alt = (int)((h >> 16) % (uint32_t)(n - 1));
        return alt >= k.base ? alt + 1 : alt;
    }

    void trigger(int v, float vel, int step, uint32_t round)
    {
        Kit* k = kit_[v].load(std::memory_order_acquire);
        if (!k || k->samples.empty()) return;
        int si = pickSample(v, *k, step, round);
        const Sample& s = k->samples[(size_t)clamp(si, 0, (int)k->samples.size() - 1)];
        if (!s.frames()) return;
        // the voice's other player fades out (a drum cuts itself); the closed hat cuts the open hat
        for (Player& q : players_[v]) if (q.active && q.fade < 0) q.fade = 64;
        if (v == 4 && getBool(P_CHOKE)) for (Player& q : players_[5]) if (q.active && q.fade < 0) q.fade = 96;
        Player* p = &players_[v][0];
        for (Player& q : players_[v]) if (!q.active) { p = &q; break; }
        if (p->active) p = &players_[v][1];
        const float tune = (funcValue(v, F_TUNE, 50.f) - 50.f) * 0.24f;
        p->s = &s;
        p->rev = funcValue(v, F_REVERSE, 0.f) >= 50.f;
        p->rate = s.rate / sr_ * std::pow(2.0, tune / 12.0);
        p->end = (double)s.frames() - 1.0;
        p->pos = p->rev ? p->end - 1.0 : 0.0;
        float dec = funcValue(v, F_DECAY, 100.f);
        p->decayLen = dec >= 99.5f ? 0.f : (float)(std::max(0.03f, dec / 100.f) * s.frames() / p->rate);
        p->drive = funcValue(v, F_DRIVE, 0.f) / 100.f;
        p->bits = bitsOf(funcValue(v, F_BITS, 100.f));
        float fc = funcValue(v, F_FILTER, 100.f);
        p->fa = fc >= 99.5f ? 1.f : 1.f - std::exp(-2.f * 3.14159265f * cutoffHz(fc) / (float)sr_);
        p->zl = p->zr = 0.f;
        float pan = (funcValue(v, F_PAN, 50.f) - 50.f) / 50.f;
        p->panL = std::sqrt(0.5f * (1.f - pan)) * 1.4142f;
        p->panR = std::sqrt(0.5f * (1.f + pan)) * 1.4142f;
        p->gain = 0.9f * (0.15f + 0.85f * vel) * levelGain_[v];
        p->age = 0;
        p->fade = -1;
        p->active = true;
        if (step >= 0 && si != k->base) swapped_[v].fetch_or(1ull << step, std::memory_order_relaxed);
    }

    bool sampleAt(Player& p, float& a, float& b) const
    {
        const Sample& s = *p.s;
        if (p.rev ? p.pos <= 0.0 : p.pos >= p.end) return false;
        size_t k = (size_t)p.pos;
        if (k + 1 >= s.frames()) return false;
        float f = (float)(p.pos - (double)k);
        a = (s.L[k] + (s.L[k + 1] - s.L[k]) * f) / 32768.f;
        if (s.R.empty()) b = a;
        else b = (s.R[k] + (s.R[k + 1] - s.R[k]) * f) / 32768.f;
        return true;
    }

    // ------------------------------------------------------------------ worker: folders
    static Sample toSample(const Wave& w, const std::string& name)
    {
        Sample s;
        s.rate = w.rate;
        s.name = name;
        s.L.resize(w.frames());
        for (size_t i = 0; i < w.frames(); ++i) s.L[i] = (int16_t)std::lround(std::max(-1.f, std::min(1.f, w.L[i])) * 32767.f);
        if (!w.R.empty())
        {
            s.R.resize(w.frames());
            for (size_t i = 0; i < w.frames(); ++i) s.R[i] = (int16_t)std::lround(std::max(-1.f, std::min(1.f, w.R[i])) * 32767.f);
        }
        return s;
    }

    // load a voice's folder (up to kMaxFiles WAVs, the chosen one always among them)
    bool loadVoice(int v, const std::string& folder, const std::string& file)
    {
        std::vector<std::string> files = listDir(folder, false);
        if (files.empty()) return false;
        int baseIdx = 0;
        for (size_t i = 0; i < files.size(); ++i) if (files[i] == file) baseIdx = (int)i;
        std::vector<int> take;
        if ((int)files.size() <= kMaxFiles) for (int i = 0; i < (int)files.size(); ++i) take.push_back(i);
        else
        {
            for (int i = 0; i < kMaxFiles; ++i) take.push_back((int)((long)i * (long)files.size() / kMaxFiles));
            if (std::find(take.begin(), take.end(), baseIdx) == take.end()) take[0] = baseIdx;
        }
        Kit* k = new Kit();
        for (int i : take)
        {
            Wave w;
            if (!loadWav(folder + "/" + files[(size_t)i], w, kMaxSeconds)) continue;
            if (i == baseIdx) k->base = (int)k->samples.size();
            k->samples.push_back(toSample(w, stripWav(files[(size_t)i])));
        }
        if (k->samples.empty()) { delete k; return false; }
        for (auto& r : retired_[v]) delete r.exchange(nullptr);
        delete pendingKit_[v].exchange(k, std::memory_order_acq_rel);
        size_t bytes = 0;
        for (const Sample& s : k->samples) bytes += (s.L.size() + s.R.size()) * 2;
        kitInfo_[v] = {folder, stripWav(files[(size_t)baseIdx]), (int)k->samples.size()};
        kitBytes_[v] = bytes;
        std::lock_guard<std::mutex> g(mtx_);
        voiceFolder_[v] = folder;
        voiceFile_[v] = files[(size_t)baseIdx];
        return true;
    }

    std::string folderLabel(const std::string& f) const
    {
        if (f.empty()) return "built in";
        if (!root_.empty() && f.compare(0, root_.size(), root_) == 0 && f.size() > root_.size()) return f.substr(root_.size() + 1);
        return baseName(f);
    }

    // ------------------------------------------------------------------ worker: the ORDNER page
    void tapRow(int row)
    {
        std::string path = browser_.tap(row);
        if (path.empty()) { previewPath_.clear(); return; }
        if (hasExt(path, ".kit")) { previewPath_.clear(); loadKit(path); return; }
        int v = clamp(getInt(P_VOICE), 0, kVoices - 1);
        if (path == previewPath_)
        {
            previewPath_.clear();
            size_t i = path.find_last_of('/');
            if (loadVoice(v, path.substr(0, i), path.substr(i + 1))) screen_.message(P_LISTINFO, std::string(kVoiceNames[v]) + ": " + stripWav(baseName(path)));
            else screen_.message(P_LISTINFO, "cannot read");
        }
        else
        {
            Wave w;
            if (!loadWav(path, w, 4.f)) { screen_.message(P_LISTINFO, "cannot read"); return; }
            Sample* s = new Sample(toSample(w, baseName(path)));
            for (auto& r : previewRetired_) delete r.exchange(nullptr);
            delete previewPending_.exchange(s, std::memory_order_acq_rel);
            previewPath_ = path;
            screen_.message(P_LISTINFO, "tap again = the voice's sample");
        }
    }

    // ------------------------------------------------------------------ worker: SAVE SAMPLE, SAVE KIT, kits
    // where SAVE writes: the folder open on the FOLDER page, else the voice's folder, else the root folder
    std::string saveDir(int v)
    {
        std::string d = browser_.cwd();
        if (!d.empty() && d != "*" && isDir(d)) return d;
        {
            std::lock_guard<std::mutex> g(mtx_);
            if (!voiceFolder_[v].empty() && isDir(voiceFolder_[v])) return voiceFolder_[v];
        }
        return !root_.empty() && isDir(root_) ? root_ : std::string();
    }
    static std::string freeName(const std::string& dir, const std::string& stem, const char* ext)
    {
        for (int n = 1; n < 1000; ++n)
        {
            char b[16];
            std::snprintf(b, sizeof b, " %02d", n);
            std::string f = stem + b + ext;
            if (FILE* x = std::fopen((dir + "/" + f).c_str(), "rb")) std::fclose(x);
            else return f;
        }
        return stem + ext;
    }
    static void put16(std::vector<uint8_t>& o, uint32_t v) { o.push_back((uint8_t)v); o.push_back((uint8_t)(v >> 8)); }
    static void put32(std::vector<uint8_t>& o, uint32_t v) { put16(o, v & 0xffff); put16(o, v >> 16); }

    // the voice's own sample through its functions (TUNE, REVERSE, DECAY, BITS, DRIVE, FILTER, PAN) as a 16-bit WAV
    void saveSample()
    {
        int v = clamp(getInt(P_VOICE), 0, kVoices - 1);
        Kit* k = kit_[v].load(std::memory_order_acquire); // kits are freed only by this thread
        if (!k || k->samples.empty()) { screen_.message(P_LISTINFO, "no sample"); return; }
        const Sample& s = k->samples[(size_t)clamp(k->base, 0, (int)k->samples.size() - 1)];
        std::string dir = saveDir(v);
        if (dir.empty()) { screen_.message(P_LISTINFO, "open a folder first"); return; }
        Player p;
        p.s = &s;
        const double sr = s.rate;
        p.rev = funcValue(v, F_REVERSE, 0.f) >= 50.f;
        p.rate = std::pow(2.0, (funcValue(v, F_TUNE, 50.f) - 50.f) * 0.24 / 12.0);
        p.end = (double)s.frames() - 1.0;
        p.pos = p.rev ? p.end - 1.0 : 0.0;
        float dec = funcValue(v, F_DECAY, 100.f);
        p.decayLen = dec >= 99.5f ? 0.f : (float)(std::max(0.03f, dec / 100.f) * s.frames() / p.rate);
        p.drive = funcValue(v, F_DRIVE, 0.f) / 100.f;
        p.bits = bitsOf(funcValue(v, F_BITS, 100.f));
        float fc = funcValue(v, F_FILTER, 100.f);
        p.fa = fc >= 99.5f ? 1.f : 1.f - std::exp(-2.f * 3.14159265f * cutoffHz(fc) / (float)sr);
        float pan = (funcValue(v, F_PAN, 50.f) - 50.f) / 50.f;
        p.panL = std::min(1.f, std::sqrt(0.5f * (1.f - pan)) * 1.4142f);
        p.panR = std::min(1.f, std::sqrt(0.5f * (1.f + pan)) * 1.4142f);
        std::vector<float> L, Rr;
        float a, b;
        while (sampleAt(p, a, b) && L.size() < (size_t)(sr * 8.0))
        {
            float env = 1.f;
            if (p.decayLen > 0.f) { env = std::max(0.f, 1.f - p.age / p.decayLen); if (env <= 0.f) break; }
            if (p.bits < 16) { float q = (float)(1 << (p.bits - 1)); a = std::round(a * q) / q; b = std::round(b * q) / q; }
            if (p.drive > 0.01f) { float d = 1.f + 8.f * p.drive; a = std::tanh(a * d) / std::tanh(d); b = std::tanh(b * d) / std::tanh(d); }
            if (p.fa < 1.f) { p.zl += p.fa * (a - p.zl); p.zr += p.fa * (b - p.zr); a = p.zl; b = p.zr; }
            L.push_back(a * env * p.panL);
            Rr.push_back(b * env * p.panR);
            p.pos += p.rev ? -p.rate : p.rate;
            ++p.age;
        }
        if (L.empty()) { screen_.message(P_LISTINFO, "nothing to save"); return; }
        for (size_t i = 0; i < 64 && i < L.size(); ++i) { size_t j = L.size() - 1 - i; L[j] *= i / 64.f; Rr[j] *= i / 64.f; }
        const uint32_t n = (uint32_t)L.size(), rate = (uint32_t)std::lround(sr);
        std::vector<uint8_t> w;
        w.insert(w.end(), {'R', 'I', 'F', 'F'}); put32(w, 36 + n * 4);
        w.insert(w.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '}); put32(w, 16); put16(w, 1); put16(w, 2);
        put32(w, rate); put32(w, rate * 4); put16(w, 4); put16(w, 16);
        w.insert(w.end(), {'d', 'a', 't', 'a'}); put32(w, n * 4);
        for (uint32_t i = 0; i < n; ++i)
        {
            put16(w, (uint16_t)(int16_t)std::lround(std::max(-1.f, std::min(1.f, L[i])) * 32767.f));
            put16(w, (uint16_t)(int16_t)std::lround(std::max(-1.f, std::min(1.f, Rr[i])) * 32767.f));
        }
        std::string stem = stripWav(s.name);
        if (stem.size() > 20) stem.resize(20);
        std::string f = freeName(dir, stem, ".wav");
        if (writeFileBytes(dir + "/" + f, w.data(), w.size()))
        {
            if (browser_.cwd() == dir) browser_.list(dir + "/" + f);
            screen_.message(P_LISTINFO, "saved: " + stripWav(f), 4000);
        }
        else screen_.message(P_LISTINFO, "Error while writing");
    }

    // a kit: every voice's folder and sample, RANDOM, LEVEL and its four functions
    void saveKit()
    {
        std::string dir = saveDir(clamp(getInt(P_VOICE), 0, kVoices - 1));
        if (dir.empty()) { screen_.message(P_LISTINFO, "open a folder first"); return; }
        std::string t = "SGK1\n";
        {
            std::lock_guard<std::mutex> g(mtx_);
            for (int v = 0; v < kVoices; ++v)
            {
                char b[64];
                t += voiceFolder_[v] + "\t" + voiceFile_[v];
                std::snprintf(b, sizeof b, "\t%.2f\t%.2f", get(P_RANDOM0 + v), get(P_LEVEL0 + v));
                t += b;
                for (int k = 0; k < kSlots; ++k)
                {
                    std::snprintf(b, sizeof b, "\t%d\t%.2f", getInt(P_FUNC0 + v * kSlots + k), get(P_VAL0 + v * kSlots + k));
                    t += b;
                }
                t += "\n";
            }
        }
        std::string f = freeName(dir, "Kit", ".kit");
        if (writeFileBytes(dir + "/" + f, t.data(), t.size()))
        {
            if (browser_.cwd() == dir) browser_.list(dir + "/" + f);
            screen_.message(P_LISTINFO, "saved: " + f.substr(0, f.size() - 4), 4000);
        }
        else screen_.message(P_LISTINFO, "Error while writing");
    }

    void setParam(int i, float x) { set(i, x); notifyHostParameterChanged(i); }

    void loadKit(const std::string& path)
    {
        std::string t;
        if (FILE* fp = std::fopen(path.c_str(), "rb"))
        {
            char buf[4096];
            size_t n;
            while ((n = std::fread(buf, 1, sizeof buf, fp)) > 0) t.append(buf, n);
            std::fclose(fp);
        }
        if (t.compare(0, 5, "SGK1\n") != 0) { screen_.message(P_LISTINFO, "not a SCHLAG kit"); return; }
        size_t pos = 5;
        int missing = 0;
        for (int v = 0; v < kVoices && pos < t.size(); ++v)
        {
            size_t e = t.find('\n', pos);
            std::string line = t.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
            pos = e == std::string::npos ? t.size() : e + 1;
            std::vector<std::string> c;
            size_t a = 0;
            for (;;) { size_t b = line.find('\t', a); c.push_back(line.substr(a, b == std::string::npos ? std::string::npos : b - a)); if (b == std::string::npos) break; a = b + 1; }
            if (c.size() < 4 + 2 * kSlots) continue;
            if (!c[0].empty() && !isDir(c[0])) ++missing;
            {
                std::lock_guard<std::mutex> g(mtx_);
                voiceFolder_[v] = c[0];
                voiceFile_[v] = c[1];
                loadReq_[v] = !c[0].empty();
            }
            setParam(P_RANDOM0 + v, (float)std::atof(c[2].c_str()));
            setParam(P_LEVEL0 + v, (float)std::atof(c[3].c_str()));
            for (int k = 0; k < kSlots; ++k)
            {
                setParam(P_FUNC0 + v * kSlots + k, (float)clamp(std::atoi(c[4 + 2 * k].c_str()), 0, NUM_FUNCS - 1));
                setParam(P_VAL0 + v * kSlots + k, (float)std::atof(c[5 + 2 * k].c_str()));
            }
        }
        std::string name = baseName(path);
        name = name.substr(0, name.size() - 4);
        screen_.message(P_LISTINFO, missing ? name + ": " + std::to_string(missing) + " folders missing" : name + " loaded", 4000);
    }

    // ------------------------------------------------------------------ worker: screen
    long long nowMs() const
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    void texts(genralink::LinkData* L, bool alive)
    {
        const char* kn[12] = {"C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};
        int kr = L ? L->keyRoot.load() : -1;
        std::string key = kr >= 0 && kr < 12 ? std::string(kn[kr]) + (L->keyMinor.load() ? "m" : "") : std::string("-");
        int bpm100 = L ? L->bpmX100.load() : 0;
        char b[64];
        if (alive) std::snprintf(b, sizeof b, "LINK GENRA CHOP · %.0f BPM", bpm100 / 100.0);
        else std::snprintf(b, sizeof b, "no GENRA CHOP");
        screen_.setText(P_HDRLINK, b);
        for (int v = 0; v < kVoices; ++v)
        {
            screen_.setText(P_FOLDERTXT0 + v, folderLabel(kitInfo_[v].folder));
            screen_.setText(P_SAMPLETXT0 + v, kitInfo_[v].file);
        }
        int sv = clamp(getInt(P_VOICE), 0, kVoices - 1);
        screen_.setText(P_VOICEINFO, std::string("Voice: ") + kVoiceNames[sv]);
        screen_.setText(P_VFOLDER, "Folder: " + folderLabel(kitInfo_[sv].folder));
        screen_.setText(P_VSAMPLE, "Sample: " + kitInfo_[sv].file);
        screen_.setText(P_VCOUNT, std::to_string(kitInfo_[sv].count) + " samples to roll");
        screen_.setText(P_PATH, browser_.pathText(), true);
        screen_.setText(P_LISTINFO, browser_.info());
        std::string loaded;
        {
            std::lock_guard<std::mutex> g(mtx_);
            loaded = voiceFolder_[sv].empty() ? std::string() : voiceFolder_[sv] + "/" + voiceFile_[sv];
        }
        for (int r = 0; r < kRows; ++r) screen_.setText(P_ROWT0 + r, browser_.rowText(r, loaded, previewPath_));
        std::snprintf(b, sizeof b, "Drums · 4 bars · %.0f BPM · %s", bpm100 / 100.0, key.c_str());
        screen_.setText(P_PRINTINFO, b);
        screen_.setText(P_PRINTSTATUS, getInt(P_AUTO) == 0 ? "AUTO: the loop plays" : "PADS: only the MPC track's notes");
        screen_.setText(P_LEARNINFO, pads_.learning() ? pads_.info() : (pads_.learned() ? "learned" : "not learned yet"));
        screen_.setText(P_MAPINFO, pads_.info());
        screen_.setText(P_ROOTTXT, root_.empty() ? std::string("none yet (FOLDER: ROOT HERE)") : pathLabel(root_), true);
        screen_.setText(P_LINKINFO, !L ? "Error: " + link_.error() : (alive ? std::string("GENRA CHOP linked") : std::string("no GENRA CHOP")));
        size_t bytes = 0;
        for (size_t x : kitBytes_) bytes += x;
        std::snprintf(b, sizeof b, "%.1f MB samples loaded", bytes / 1048576.0);
        screen_.setText(P_MEMINFO, b);
        int step = curStep_.load();
        screen_.setText(P_STATUS, step >= 0 ? "Bar " + std::to_string(step / 16 + 1) + " / 4" : std::string(getInt(P_AUTO) == 0 ? "AUTO" : "PADS"));
    }

    void cells()
    {
        const genralink::Loop& l = loop_;
        int step = curStep_.load();
        int bar = step >= 0 ? step / 16 : 0;
        uint32_t lit = litMask_.load();
        for (int v = 0; v < kVoices; ++v)
        {
            uint64_t sw = swapped_[v].load();
            for (int j = 0; j < 16; ++j)
            {
                int s = bar * 16 + j;
                bool hit = l.drum[v][s].vel > 0;
                bool rnd = hit && (sw >> s & 1u);
                // without the loop playing: which hits would swap (the same roll the audio thread uses)
                if (hit && step < 0)
                {
                    Kit* k = kit_[v].load();
                    if (k && k->samples.size() > 1) rnd = pickSample(v, *k, s, 0) != k->base;
                }
                screen_.setFrame(P_CELL0 + v * 16 + j, hit ? (rnd ? 2 : 1) : 0);
            }
            screen_.setFrame(P_LAMP0 + v, (lit >> v) & 1u);
            screen_.setFrame(P_PADLIT0 + v, (lit >> v) & 1u);
        }
    }

    void print(genralink::LinkData* L)
    {
        std::string dir = printDir(std::string());
        if (dir.empty()) { screen_.message(P_PRINTSTATUS, "no folder to write to"); return; }
        const genralink::Loop& l = loop_;
        const float swing = l.swingX1000 / 1000.f;
        bool solo = false;
        for (int v = 0; v < kVoices; ++v) if (getBool(P_SOLO0 + v)) solo = true;
        std::vector<MidiNote> notes;
        for (int v = 0; v < kVoices; ++v)
        {
            if (getBool(P_MUTE0 + v) || (solo && !getBool(P_SOLO0 + v))) continue;
            int rep = repeatOf(v);
            for (int s = 0; s < kSteps; ++s)
            {
                const genralink::DrumHit& h = l.drum[v][s];
                if (!h.vel || h.chance < 128) continue;
                double at = s + ((s & 1) ? (swing - 0.5f) * 2.f : 0.f) + std::max(0, (int)h.off) / 100.0;
                int r = std::max(std::max(1, (int)h.ratchet), rep);
                for (int k = 0; k < r; ++k)
                    notes.push_back({at + (double)k / r, 0.25 / r, pads_.noteOf(v), (int)(h.vel * (k ? 0.75f : 1.f))});
            }
        }
        int bpm100 = L ? L->bpmX100.load() : 0;
        double bpm = bpm100 > 0 ? bpm100 / 100.0 : 120.0;
        int no = printNumberFor(dir, l.id);
        char name[96];
        std::snprintf(name, sizeof name, "Print %02d Drums %.0f", no, bpm);
        std::vector<uint8_t> f = midiFile(notes, bpm, name, kSteps);
        if (writeFileBytes(dir + "/" + name + ".mid", f.data(), f.size()))
        {
            set(P_AUTO, 1.f);
            notifyHostParameterChanged(P_AUTO);
            screen_.message(P_PRINTSTATUS, std::string(name) + " saved, now PADS", 4000);
        }
        else screen_.message(P_PRINTSTATUS, "Error while writing");
        printList();
    }

    void printList()
    {
        std::string dir = printDir(std::string());
        screen_.setText(P_PRINTDIRTXT, dir.empty() ? std::string("no folder") : pathLabel(dir), true);
        std::vector<std::string> list = dir.empty() ? std::vector<std::string>() : listPrints(dir);
        for (int r = 0; r < kPrintRows; ++r)
            screen_.setText(P_PRINTROW0 + r, r < (int)list.size() ? (r == 0 ? "> " : "") + list[(size_t)r].substr(0, list[(size_t)r].size() - 4) : std::string());
    }

    void workerMain()
    {
        root_ = settings_.get("root");
        browser_.rows = kRows;
        browser_.extra = ".kit";
        browser_.open(!root_.empty() ? root_ : settings_.get("lastdir"));
        uint32_t lastSeq = 1;
        genralink::Loop tmp;
        genralink::Link::clearLoop(loop_);
        genralink::Link::Alive alive;
        float lastRandom[kVoices];
        for (int v = 0; v < kVoices; ++v) lastRandom[v] = get(P_RANDOM0 + v);
        long long lastSlow = 0;
        while (!quit_.load())
        {
            genralink::LinkData* L = link_.data();
            if (resyncReq_.exchange(false)) screen_.resync();
            for (int v = 0; v < kVoices; ++v)
            {
                bool want = false;
                std::string folder, file;
                {
                    std::lock_guard<std::mutex> g(mtx_);
                    if (loadReq_[v]) { want = true; loadReq_[v] = false; folder = voiceFolder_[v]; file = voiceFile_[v]; }
                }
                if (want && !folder.empty() && !loadVoice(v, folder, file)) screen_.message(P_STATUS, std::string(kVoiceNames[v]) + ": folder missing");
            }
            bool on = L && alive.check(L->writerBeat.load(), nowMs());
            if (L)
            {
                uint32_t seq = 0;
                if (L->magic.load() == genralink::kMagic && L->loopSeq.load() != lastSeq && link_.readLoop(tmp, seq) && seq != 0)
                {
                    lastSeq = seq;
                    loop_ = tmp;
                    int r = ready_.load(), u = inUse_.load(), w = 0;
                    while (w == r || w == u) ++w;
                    loops_[w] = tmp;
                    ready_.store(w, std::memory_order_release);
                    for (auto& s : swapped_) s.store(0);
                }
            }
            // RANDOM: every turn rolls again
            for (int v = 0; v < kVoices; ++v)
            {
                float r = get(P_RANDOM0 + v);
                if (std::fabs(r - lastRandom[v]) > 0.4f) { lastRandom[v] = r; seed_[v].fetch_add(1); swapped_[v].store(0); }
            }
            for (auto& t : screen_.taps())
            {
                const int p = t.first;
                if (p == P_NEW && L) { L->lane[genralink::LANE_DRUMS].newRequest.fetch_add(1); screen_.message(P_STATUS, on ? "NEW: new drums from the next bar" : "no GENRA CHOP"); }
                else if (p == P_UNDO && L) { L->lane[genralink::LANE_DRUMS].undoRequest.fetch_add(1); screen_.message(P_STATUS, on ? "UNDO: the last drums" : "no GENRA CHOP"); }
                else if (p == P_BASIC)
                {
                    for (int v = 0; v < kVoices; ++v) { set(P_RANDOM0 + v, 0.f); notifyHostParameterChanged(P_RANDOM0 + v); lastRandom[v] = 0.f; swapped_[v].store(0); }
                    screen_.message(P_STATUS, "BASIC: every voice on its sample");
                }
                else if (p == P_PRINT) print(L);
                else if (p == P_SAVESMP) saveSample();
                else if (p == P_SAVEKIT) saveKit();
                else if (p == P_LEARN) pads_.startLearning();
                else if (p == P_BACK) { browser_.up(); previewPath_.clear(); }
                else if (p == P_DRVBTN) { browser_.drives(); previewPath_.clear(); }
                else if (p == P_PGUP) browser_.page(-1);
                else if (p == P_PGDN) browser_.page(1);
                else if (p == P_SCROLL) browser_.scroll(t.second > 0 ? 1 + (t.second - 1) / 6 : -1 - (-t.second - 1) / 6);
                else if (p == P_ROOTHERE)
                {
                    if (browser_.cwd().empty() || browser_.cwd() == "*") screen_.message(P_LISTINFO, "open a folder first");
                    else { root_ = browser_.cwd(); settings_.set("root", root_); settings_.save(); screen_.message(P_LISTINFO, "Root folder set"); }
                }
                else if (p >= P_FUNCTAP0 && p < P_FUNCTAP0 + kVoices * kSlots)
                {
                    int idx = p - P_FUNCTAP0;
                    int f = (getInt(P_FUNC0 + idx) + 1) % NUM_FUNCS;
                    set(P_FUNC0 + idx, (float)f);
                    set(P_VAL0 + idx, defaultValue(f));
                    notifyHostParameterChanged(P_FUNC0 + idx);
                    notifyHostParameterChanged(P_VAL0 + idx);
                }
                else if (p >= P_PICK0 && p < P_PICK0 + kVoices)
                {
                    int v = p - P_PICK0;
                    set(P_VOICE, (float)v);
                    notifyHostParameterChanged(P_VOICE);
                    std::string f;
                    {
                        std::lock_guard<std::mutex> g(mtx_);
                        f = voiceFolder_[v];
                    }
                    if (!f.empty()) browser_.open(f);
                    screen_.message(P_STATUS, std::string(kVoiceNames[v]) + " chosen: page FOLDER");
                }
                else if (p >= P_ROWTAP0 && p < P_ROWTAP0 + kRows) tapRow(p - P_ROWTAP0);
            }
            if (pads_.finishIfDone()) screen_.message(P_LEARNINFO, "learned and saved", 3000);
            if (nowMs() - lastSlow > 1500)
            {
                lastSlow = nowMs();
                printList();
                if (L) genralink::Link::writeName(L->lane[genralink::LANE_DRUMS], "SCHLAG");
                if (browser_.cwd().empty()) browser_.list();
            }
            texts(L, on);
            cells();
            screen_.push();
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
        }
    }

    struct KitInfo { std::string folder, file; int count = 0; };

    genraui::Screen screen_;
    genralink::Link link_;
    PadMap pads_;
    Settings settings_{"SCHLAG.txt"};
    Browser browser_;
    bool started_ = false;
    std::thread worker_;
    std::atomic<bool> quit_{false}, resyncReq_{false};
    // audio thread
    Player players_[kVoices][2];
    Hit hits_[64];
    int nHits_ = 0;
    double sr_ = 44100.0, nextStep_ = 0.0, previewPos_ = 0.0;
    bool wasPlaying_ = false;
    bool audible_[kVoices] = {};
    float levelGain_[kVoices] = {1, 1, 1, 1, 1, 1, 1, 1};
    float gainSmooth_ = -1.f, limGain_ = 1.f;
    int padSeen_[kVoices] = {};
    // shared
    std::atomic<Kit*> kit_[kVoices], pendingKit_[kVoices];
    std::atomic<Kit*> retired_[kVoices][3];
    std::atomic<Sample*> preview_, previewPending_{nullptr};
    std::atomic<Sample*> previewRetired_[3];
    std::atomic<uint32_t> seed_[kVoices];
    std::atomic<uint64_t> swapped_[kVoices];
    std::atomic<uint32_t> litMask_{0};
    std::atomic<int> ready_{-1}, inUse_{-1}, curStep_{-1};
    genralink::Loop loops_[3];
    int cur_ = -1; // audio thread: the loop that plays (index into loops_)
    // worker
    genralink::Loop loop_;
    KitInfo kitInfo_[kVoices];
    size_t kitBytes_[kVoices] = {};
    std::string root_, previewPath_;
    // worker + state (mtx_)
    std::mutex mtx_;
    std::string voiceFolder_[kVoices], voiceFile_[kVoices];
    bool loadReq_[kVoices] = {};
};

MPC_PLUGIN_ENTRY(Schlag)
