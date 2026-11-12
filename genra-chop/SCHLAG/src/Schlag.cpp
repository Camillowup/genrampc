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

#include <chrono>
#include <cmath>
#include <memory>
#include <mutex>
#include <thread>

using namespace mpc;
using namespace genrafiles;

namespace {

static const int kVoices = 8, kSlots = 4, kSteps = genralink::kSteps, kRows = 10, kPrintRows = 7;
static const int kMaxFiles = 24;
static const float kMaxSeconds = 2.f;
const char* const kVoiceNames[kVoices] = {"KICK", "SNARE", "CLAP", "RIM", "HAT ZU", "HAT OFFEN", "TOM", "PERC"};
enum Func { F_TUNE, F_DECAY, F_DRIVE, F_BITS, F_REPEAT, F_PAN, F_REVERSE, F_FILTER, NUM_FUNCS };
const char* const kFuncNames[NUM_FUNCS] = {"TUNE", "DECAY", "DRIVE", "BITS", "REPEAT", "PAN", "REVERSE", "FILTER"};
const int kDefaultFunc[kSlots] = {F_TUNE, F_DECAY, F_DRIVE, F_PAN};

static const char* const kCaptions[][2] = {
    {"schlag", "SCHLAG"}, {"auto", "AUTO"}, {"padsmode", "PADS"}, {"undo", "UNDO"}, {"basic", "BASIC"}, {"new", "NEW"},
    {"m", "M"}, {"s", "S"}, {"v1", "KICK"}, {"v2", "SNARE"}, {"v3", "CLAP"}, {"v4", "RIM"}, {"v5", "HAT ZU"},
    {"v6", "HAT OFFEN"}, {"v7", "TOM"}, {"v8", "PERC"}, {"p1", "1"}, {"p2", "2"}, {"p3", "3"}, {"p4", "4"},
    {"p5", "5"}, {"p6", "6"}, {"p7", "7"}, {"p8", "8"}, {"ordner", "· ORDNER"}, {"back", "ORDNER ▲"},
    {"drives", "LAUFWERKE"}, {"pgup", "< SEITE"}, {"pgdn", "SEITE >"}, {"root", "HAUPTORDNER HIER"},
    {"hint1", "1. Tipp = hoeren"}, {"hint2", "2. Tipp = Sample der Stimme"}, {"print", "PRINT"},
    {"printtitle", "· PRINT"}, {"after", "Nach PRINT"}, {"autoinfo", "AUTO: der Loop spielt"},
    {"padsinfo", "PADS: nur die Noten der MPC-Spur"}, {"learn", "LERNEN"}, {"help1", "LERNEN antippen, dann die"},
    {"help2", "Pads 1 bis 16 der Reihe nach"}, {"help3", "druecken (Pad 1 = unten links)"},
    {"help4", "Gilt fuer alle GENRA-Plugins"}, {"choke", "HAT-CHOKE"}, {"r1", "Pad-Belegung"}, {"r2", "Hauptordner"},
    {"r3", "LINK"}, {"r4", "Speicher"}, {"r5", "Version"}, {"setup", "· SETUP"}};
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
    P_VOLUME = P_VAL0 + kVoices * kSlots, P_AUTO, P_CHOKE, P_VOICE,
    // buttons
    P_NEW, P_UNDO, P_BASIC, P_PRINT, P_LEARN, P_BACK, P_DRVBTN, P_PGUP, P_PGDN, P_ROOTHERE, // flashes 0..9
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
    P_COUNT = P_FLASH0 + 10
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
    s.name = std::string(kVoiceNames[voice]) + " (eingebaut)";
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
                addParam(choiceParam(("func" + std::to_string(v + 1) + "_" + std::to_string(k + 1)).c_str(), ("FUNKTION " + std::to_string(v + 1) + " " + std::to_string(k + 1)).c_str(), funcs, kDefaultFunc[k]));
        for (int v = 0; v < kVoices; ++v)
            for (int k = 0; k < kSlots; ++k)
                addParam(floatParam(("val" + std::to_string(v + 1) + "_" + std::to_string(k + 1)).c_str(), ("WERT " + std::to_string(v + 1) + " " + std::to_string(k + 1)).c_str(), 0.f, 100.f, defaultValue(kDefaultFunc[k]), "", 0));
        addParam(floatParam("volume", "VOLUME", -40.f, 6.f, 0.f, "dB", 1));
        addParam(choiceParam("auto", "AUTO PADS", {"Auto", "Pads"}, 0));
        addParam(boolParam("choke", "HAT CHOKE", true));
        addParam(choiceParam("voice", "STIMME", {"KICK", "SNARE", "CLAP", "RIM", "HAT ZU", "HAT OFFEN", "TOM", "PERC"}, 0));
        for (const char* b : {"NEW", "UNDO", "BASIC", "PRINT", "LERNEN", "ORDNER HOCH", "LAUFWERKE", "SEITE ZURUECK", "SEITE VOR", "HAUPTORDNER HIER"})
            addButton(b);
        addParam(intParam("scroll", "LISTE", 0, 1000, 500));
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
        for (int i = 0; i < 10; ++i) addText(("BLITZ " + std::to_string(i + 1)).c_str());
        if (numParams() != P_COUNT) std::abort();

        screen_.init();
        for (int i = P_HDRLINK; i < P_CELL0; ++i) screen_.text(i);
        for (int i = 0; i < kNumCaps; ++i) screen_.text(P_CAP0 + i, kCaptions[i][1]);
        for (int i = 0; i < kVoices * 16; ++i) screen_.strip(P_CELL0 + i, 3);
        for (int v = 0; v < kVoices; ++v) { screen_.strip(P_LAMP0 + v, 2); screen_.strip(P_PADLIT0 + v, 2); }
        for (int i = 0; i < 10; ++i) screen_.button(P_NEW + i, P_FLASH0 + i);
        for (int i = P_FUNCTAP0; i < P_ROWTAP0 + kRows; ++i) if (i < P_PADTAP0 || i >= P_ROWTAP0) screen_.button(i);
        screen_.watch(P_SCROLL);
        screen_.watch(P_AUTO);
        screen_.setText(P_VERSION, "SCHLAG 0.1 · LINK v3");

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
        i.version = 100;
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
                kitInfo_[v] = {std::string(), "eingebaut", 1};
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
        int r = ready_.load(std::memory_order_acquire);
        if (r >= 0) inUse_.store(r, std::memory_order_release);
        const genralink::Loop* loop = r >= 0 ? &loops_[r] : nullptr;

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
        if (playing && autoMode && loop)
        {
            const double q0 = tr.ppqPos * 4.0, q1 = q0 + ctx.numFrames / spStep;
            if (!wasPlaying_ || nextStep_ < q0 - 1.0 || nextStep_ > q1 + 1.0) nextStep_ = std::ceil(q0 - 1e-6);
            const float swing = loop->swingX1000 / 1000.f;
            while (nextStep_ < q1)
            {
                long n = (long)std::llround(nextStep_);
                int s = (int)(((n % kSteps) + kSteps) % kSteps);
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
        for (int i = 0; i < ctx.numFrames; ++i)
        {
            while (ev < ctx.numMidiEvents && ctx.midi[ev].sampleOffset <= i)
            {
                const MidiEvent& e = ctx.midi[ev++];
                if (!e.isNoteOn()) continue;
                if (pads_.learnNote(e.note())) continue;
                int pad = pads_.padOf(e.note());
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
        inUse_.store(-1, std::memory_order_release);
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
            case F_DECAY: std::snprintf(b, sizeof b, v >= 99.5f ? "voll" : "%.0f %%", v); break;
            case F_DRIVE: std::snprintf(b, sizeof b, "%.0f %%", v); break;
            case F_BITS: std::snprintf(b, sizeof b, "%d", bitsOf(v)); break;
            case F_REPEAT: { const char* r[4] = {"aus", "1/32", "1/48", "1/64"}; std::snprintf(b, sizeof b, "%s", r[repeatIndex(v)]); break; }
            case F_PAN: { int p = (int)std::lround((v - 50.f) * 2.f); if (!p) std::snprintf(b, sizeof b, "Mitte"); else std::snprintf(b, sizeof b, "%s %d", p < 0 ? "L" : "R", std::abs(p)); break; }
            case F_REVERSE: std::snprintf(b, sizeof b, v >= 50.f ? "an" : "aus"); break;
            default: if (v >= 99.5f) std::snprintf(b, sizeof b, "offen"); else std::snprintf(b, sizeof b, "LP %.1fk", cutoffHz(v) / 1000.f); break;
        }
        return b;
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
        uint32_t h = (uint32_t)step * 2654435761u ^ seed * 40503u ^ (uint32_t)v * 9176u;
        h ^= h >> 15; h *= 0x2c1b3c6du; h ^= h >> 12;
        (void)round;
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
        if (f.empty()) return "eingebaut";
        if (!root_.empty() && f.compare(0, root_.size(), root_) == 0 && f.size() > root_.size()) return f.substr(root_.size() + 1);
        return baseName(f);
    }

    // ------------------------------------------------------------------ worker: the ORDNER page
    void tapRow(int row)
    {
        std::string path = browser_.tap(row);
        if (path.empty()) { previewPath_.clear(); return; }
        int v = clamp(getInt(P_VOICE), 0, kVoices - 1);
        if (path == previewPath_)
        {
            previewPath_.clear();
            size_t i = path.find_last_of('/');
            if (loadVoice(v, path.substr(0, i), path.substr(i + 1))) screen_.message(P_LISTINFO, std::string(kVoiceNames[v]) + ": " + stripWav(baseName(path)));
            else screen_.message(P_LISTINFO, "nicht lesbar");
        }
        else
        {
            Wave w;
            if (!loadWav(path, w, 4.f)) { screen_.message(P_LISTINFO, "nicht lesbar"); return; }
            Sample* s = new Sample(toSample(w, baseName(path)));
            for (auto& r : previewRetired_) delete r.exchange(nullptr);
            delete previewPending_.exchange(s, std::memory_order_acq_rel);
            previewPath_ = path;
            screen_.message(P_LISTINFO, "nochmal tippen = Sample der Stimme");
        }
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
        else std::snprintf(b, sizeof b, "kein GENRA CHOP");
        screen_.setText(P_HDRLINK, b);
        for (int v = 0; v < kVoices; ++v)
        {
            screen_.setText(P_FOLDERTXT0 + v, folderLabel(kitInfo_[v].folder));
            screen_.setText(P_SAMPLETXT0 + v, kitInfo_[v].file);
        }
        int sv = clamp(getInt(P_VOICE), 0, kVoices - 1);
        screen_.setText(P_VOICEINFO, std::string("Stimme: ") + kVoiceNames[sv]);
        screen_.setText(P_VFOLDER, "Ordner: " + folderLabel(kitInfo_[sv].folder));
        screen_.setText(P_VSAMPLE, "Sample: " + kitInfo_[sv].file);
        screen_.setText(P_VCOUNT, std::to_string(kitInfo_[sv].count) + " Samples zum Wuerfeln");
        screen_.setText(P_PATH, browser_.pathText(), true);
        screen_.setText(P_LISTINFO, browser_.info());
        std::string loaded;
        {
            std::lock_guard<std::mutex> g(mtx_);
            loaded = voiceFolder_[sv].empty() ? std::string() : voiceFolder_[sv] + "/" + voiceFile_[sv];
        }
        for (int r = 0; r < kRows; ++r) screen_.setText(P_ROWT0 + r, browser_.rowText(r, loaded, previewPath_));
        std::snprintf(b, sizeof b, "Drums · 4 Takte · %.0f BPM · %s", bpm100 / 100.0, key.c_str());
        screen_.setText(P_PRINTINFO, b);
        screen_.setText(P_PRINTSTATUS, getInt(P_AUTO) == 0 ? "AUTO: der Loop spielt" : "PADS: nur die Noten der MPC-Spur");
        screen_.setText(P_LEARNINFO, pads_.learning() ? pads_.info() : (pads_.learned() ? "gelernt" : "noch nicht gelernt"));
        screen_.setText(P_MAPINFO, pads_.info());
        screen_.setText(P_ROOTTXT, root_.empty() ? std::string("noch keiner (ORDNER: HAUPTORDNER HIER)") : pathLabel(root_), true);
        screen_.setText(P_LINKINFO, !L ? "Fehler: " + link_.error() : (alive ? std::string("GENRA CHOP verbunden") : std::string("kein GENRA CHOP")));
        size_t bytes = 0;
        for (size_t x : kitBytes_) bytes += x;
        std::snprintf(b, sizeof b, "%.1f MB Samples geladen", bytes / 1048576.0);
        screen_.setText(P_MEMINFO, b);
        int step = curStep_.load();
        screen_.setText(P_STATUS, step >= 0 ? "Takt " + std::to_string(step / 16 + 1) + " / 4" : std::string(getInt(P_AUTO) == 0 ? "AUTO" : "PADS"));
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
        if (dir.empty()) { screen_.message(P_PRINTSTATUS, "kein Ordner zum Schreiben"); return; }
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
            screen_.message(P_PRINTSTATUS, std::string(name) + " gespeichert, jetzt PADS", 4000);
        }
        else screen_.message(P_PRINTSTATUS, "Fehler beim Schreiben");
        printList();
    }

    void printList()
    {
        std::string dir = printDir(std::string());
        screen_.setText(P_PRINTDIRTXT, dir.empty() ? std::string("kein Ordner") : pathLabel(dir), true);
        std::vector<std::string> list = dir.empty() ? std::vector<std::string>() : listPrints(dir);
        for (int r = 0; r < kPrintRows; ++r)
            screen_.setText(P_PRINTROW0 + r, r < (int)list.size() ? (r == 0 ? "> " : "") + list[(size_t)r].substr(0, list[(size_t)r].size() - 4) : std::string());
    }

    void workerMain()
    {
        root_ = settings_.get("root");
        browser_.rows = kRows;
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
                if (want && !folder.empty() && !loadVoice(v, folder, file)) screen_.message(P_STATUS, std::string(kVoiceNames[v]) + ": Ordner fehlt");
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
                if (p == P_NEW && L) { L->lane[genralink::LANE_DRUMS].newRequest.fetch_add(1); screen_.message(P_STATUS, on ? "NEW: neue Drums ab dem naechsten Takt" : "kein GENRA CHOP"); }
                else if (p == P_UNDO && L) { L->lane[genralink::LANE_DRUMS].undoRequest.fetch_add(1); screen_.message(P_STATUS, on ? "UNDO: vorige Drums" : "kein GENRA CHOP"); }
                else if (p == P_BASIC)
                {
                    for (int v = 0; v < kVoices; ++v) { set(P_RANDOM0 + v, 0.f); notifyHostParameterChanged(P_RANDOM0 + v); lastRandom[v] = 0.f; swapped_[v].store(0); }
                    screen_.message(P_STATUS, "BASIC: alle Stimmen auf ihr Sample");
                }
                else if (p == P_PRINT) print(L);
                else if (p == P_LEARN) pads_.startLearning();
                else if (p == P_BACK) { browser_.up(); previewPath_.clear(); }
                else if (p == P_DRVBTN) { browser_.drives(); previewPath_.clear(); }
                else if (p == P_PGUP) browser_.page(-1);
                else if (p == P_PGDN) browser_.page(1);
                else if (p == P_SCROLL) browser_.scroll(t.second > 0 ? 1 + (t.second - 1) / 6 : -1 - (-t.second - 1) / 6);
                else if (p == P_ROOTHERE)
                {
                    if (browser_.cwd().empty() || browser_.cwd() == "*") screen_.message(P_LISTINFO, "erst einen Ordner oeffnen");
                    else { root_ = browser_.cwd(); settings_.set("root", root_); settings_.save(); screen_.message(P_LISTINFO, "Hauptordner gesetzt"); }
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
                    screen_.message(P_STATUS, std::string(kVoiceNames[v]) + " gewaehlt: Seite ORDNER");
                }
                else if (p >= P_ROWTAP0 && p < P_ROWTAP0 + kRows) tapRow(p - P_ROWTAP0);
            }
            if (pads_.finishIfDone()) screen_.message(P_LEARNINFO, "gelernt und gespeichert", 3000);
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
