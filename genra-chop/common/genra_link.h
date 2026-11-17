// genra_link.h - LINK v3: the shared memory between GENRA CHOP (the chef) and its companions
// (SCHLAG), each on its own MPC track.
//
// SPDX-License-Identifier: GPL-3.0-only
//
// All plugins run inside the MPC application and meet in one small file mapped into memory (mmap, MAP_SHARED):
// /dev/shm/genra-link-v3, or /tmp/genra-link-v3. Only open/ftruncate/mmap from libc are used (no librt).
//
// GENRA CHOP writes the key, the chords, the style and the rendered loop (4 bars of 16 steps) of every track:
// tone tracks as notes (pitch, length, velocity, accent, slide, priority), the drums as hits per voice
// (velocity, timing, ratchets, chance, every-2nd), the chop track as a step mask. The loop is written by GENRA CHOP's
// worker under a sequence counter (seqlock); companions copy it in a non-audio thread. Everything else is atomic.
//
// A companion has a slot per role (LANE_DRUMS .. LANE_PADS): it beats there (alive), writes its sound's name and
// bumps newRequest / undoRequest when its own NEW / UNDO is pressed; GENRA CHOP then rolls that track again or
// brings its last one back.
//
// v3 (GENRA CHOP 0.2): drums complete, note priority, chop mask, undo requests. Every plugin of the set must speak
// the same version: they are updated together.
//
// Open the link from the constructor / prepare() / a worker, never from process().

#ifndef GENRA_LINK_H
#define GENRA_LINK_H

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>

#if !defined(_WIN32)
  #include <fcntl.h>
  #include <sys/mman.h>
  #include <sys/stat.h>
  #include <unistd.h>
#endif

namespace genralink {

static const uint32_t kMagic = 0x474E4C33u; // "GNL3"
static const uint32_t kVersion = 3;
static const int kChops = 64;
static const int kSteps = 64;   // 4 bars of 16
static const int kBars = 4;
static const int kDrums = 8;    // GENRA order: kick, snare, clap, rim, closed hat, open hat, tom, perc
enum Lane { LANE_DRUMS = 0, LANE_BASS, LANE_MEL, LANE_PADS, NUM_LANES };

// one note of a tone track (bass, melody), one step
struct StepNote
{
    int8_t  pitch;  // MIDI note, -1: none
    uint8_t len16;  // length in 16ths of a step (16 = one step)
    uint8_t vel;    // 1..127
    uint8_t flags;  // 1 accent, 2 slide
    uint8_t prio;   // 0..255: DENSITY keeps a note while prio <= density
    int8_t  off;    // timing, in 1/100 of a step
};

// one drum hit, one voice, one step
struct DrumHit
{
    uint8_t vel;     // 0: none
    int8_t  off;     // timing, in 1/100 of a step (-45 .. 45)
    uint8_t ratchet; // hits in the step
    uint8_t chance;  // 0..255 (255 = always)
    uint8_t cond;    // 1: only every 2nd time round
    uint8_t prio;
};

// the loop, copied as a whole under the seqlock
struct Loop
{
    StepNote bass[kSteps];
    StepNote mel[kSteps];
    int8_t   pad[kSteps][4];   // pad chord voices, -1: none (a new chord starts here)
    uint8_t  padLen[kSteps];   // in steps
    DrumHit  drum[kDrums][kSteps];
    uint8_t  chop[kSteps];     // 1: a chop starts on this step (bass RHYTHM = Chop)
    int8_t   barRoot[kBars], barQuality[kBars];
    uint16_t swingX1000;       // 500 = straight
    int16_t  style;
    uint32_t id;               // +1 for every new loop (NEW ALL); the same loop keeps its id
};

struct Companion
{
    std::atomic<uint32_t> beat;        // +1 every audio block: this companion is alive
    std::atomic<uint32_t> newRequest;  // +1 when its own NEW was pressed
    std::atomic<uint32_t> undoRequest; // +1 when its own UNDO was pressed
    std::atomic<uint32_t> nameSeq;     // seqlock for name
    char name[24];                     // its sound ("Warm Upright")
};

struct LinkData
{
    std::atomic<uint32_t> magic;
    std::atomic<uint32_t> version;
    // written by GENRA CHOP
    std::atomic<uint32_t> writerBeat;      // +1 every audio block: GENRA CHOP is alive
    std::atomic<int32_t>  keyRoot;         // 0 = C .. 11, -1 = no key
    std::atomic<int32_t>  keyMinor;        // 1 = minor
    std::atomic<int32_t>  chopCount;       // 0 = no sample
    std::atomic<int32_t>  lastChop;        // 0..63, -1 = none
    std::atomic<uint32_t> trigCount;       // +1 on every chop played (pads and loop)
    std::atomic<int32_t>  bpmX100;         // the MPC's tempo, x100
    std::atomic<int32_t>  step;            // 0..63 while the MPC plays, -1 stopped
    std::atomic<int8_t>   chordRoot[kChops];   // per chop: 0..11, -1 = no chord (a hit)
    std::atomic<int8_t>   chordQuality[kChops];
    std::atomic<uint32_t> loopSeq;         // seqlock for loop: odd while writing
    Loop                  loop;
    // written by the companions
    Companion             lane[NUM_LANES];
    // written by GENRA CHOP (added at the end, so the layout above stays): the loop's length in steps
    // (LENGTH: AUTO = the MPC sequence's loop, or 16, 32, 64, 128); a new loop starts at the start of it
    std::atomic<int32_t>  loopSteps;
    std::atomic<int32_t>  loopStart; // where that loop starts, in steps on the MPC's time line (LENGTH AUTO)
    std::atomic<uint32_t> loopEditOf; // the loopSeq of a loop that is only a hand edit: companions take it at once
    // written by GENRA SCHLAG (STEPS, HIT ON / OFF): drum edits for GENRA CHOP, a small ring
    static const int kEdits = 16;
    std::atomic<uint32_t> editW;                // +1 per edit
    std::atomic<int32_t>  editCmd[kEdits];      // voice | step << 4 | on << 12; voice 15 = CLEAR ALL (every drum hit)
    // written by GENRA SCHLAG (added at the end): 1 = TRACK (SCHLAG plays only its MPC track's notes), so GENRA CHOP
    // sends no drums on MIDI channel 10 while SCHLAG is alive
    std::atomic<int32_t>  drumsTrack;
    // written by GENRA CHOP (added at the end): the editW it has read up to, so GENRA SCHLAG does not write over
    // edits that still wait in the ring
    std::atomic<uint32_t> editR;
};
static const int kEditClearAll = 15;

class Link
{
public:
    Link() = default;
    ~Link() { close(); }
    Link(const Link&) = delete;
    Link& operator=(const Link&) = delete;

    bool open()
    {
#if defined(_WIN32)
        return false;
#else
        if (data_) return true;
        const char* candidates[] = {"/dev/shm/genra-link-v3", "/tmp/genra-link-v3"};
        for (const char* p : candidates)
        {
            int fd = ::open(p, O_RDWR | O_CREAT, 0666);
            if (fd < 0) { err_ = std::string("open ") + p + " failed"; continue; }
            struct stat st;
            if (fstat(fd, &st) == 0 && (size_t)st.st_size < sizeof(LinkData))
            {
                if (ftruncate(fd, (off_t)sizeof(LinkData)) != 0) { ::close(fd); err_ = "ftruncate failed"; continue; }
            }
            void* m = mmap(nullptr, sizeof(LinkData), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            ::close(fd);
            if (m == MAP_FAILED) { err_ = "mmap failed"; continue; }
            data_ = static_cast<LinkData*>(m);
            path_ = p;
            if (data_->magic.load() != kMagic || data_->version.load() != kVersion)
            {
                data_->keyRoot.store(-1);
                data_->lastChop.store(-1);
                data_->step.store(-1);
                for (int i = 0; i < kChops; ++i) { data_->chordRoot[i].store(-1); data_->chordQuality[i].store(-1); }
                clearLoop(data_->loop);
                data_->version.store(kVersion);
                data_->magic.store(kMagic);
            }
            return true;
        }
        return false;
#endif
    }

    static void clearLoop(Loop& l)
    {
        std::memset(&l, 0, sizeof(Loop));
        for (int s = 0; s < kSteps; ++s)
        {
            l.bass[s].pitch = l.mel[s].pitch = -1;
            for (int v = 0; v < 4; ++v) l.pad[s][v] = -1;
        }
        for (int b = 0; b < kBars; ++b) { l.barRoot[b] = -1; l.barQuality[b] = -1; }
        l.swingX1000 = 500;
    }

    void close()
    {
#if !defined(_WIN32)
        if (data_) munmap(data_, sizeof(LinkData));
#endif
        data_ = nullptr;
    }

    LinkData* data() const { return data_; }
    const std::string& path() const { return path_; }
    const std::string& error() const { return err_; }

    // GENRA CHOP's worker: publish a loop
    void writeLoop(const Loop& l)
    {
        if (!data_) return;
        data_->loopSeq.fetch_add(1, std::memory_order_acq_rel); // odd
        std::memcpy(&data_->loop, &l, sizeof(Loop));
        data_->loopSeq.fetch_add(1, std::memory_order_acq_rel); // even
    }
    // a companion's non-audio thread: copy the loop; false if none could be read
    bool readLoop(Loop& out, uint32_t& seq) const
    {
        if (!data_) return false;
        for (int tries = 0; tries < 16; ++tries)
        {
            uint32_t a = data_->loopSeq.load(std::memory_order_acquire);
            if (a & 1u) continue;
            std::memcpy(&out, &data_->loop, sizeof(Loop));
            std::atomic_thread_fence(std::memory_order_acquire);
            if (data_->loopSeq.load(std::memory_order_acquire) == a) { seq = a; return true; }
        }
        return false;
    }

    static void writeName(Companion& c, const std::string& s)
    {
        c.nameSeq.fetch_add(1, std::memory_order_acq_rel);
        std::memset(c.name, 0, sizeof(c.name));
        std::strncpy(c.name, s.c_str(), sizeof(c.name) - 1);
        c.nameSeq.fetch_add(1, std::memory_order_acq_rel);
    }
    static std::string readName(const Companion& c)
    {
        char buf[sizeof(Companion::name)];
        for (int tries = 0; tries < 8; ++tries)
        {
            uint32_t a = c.nameSeq.load(std::memory_order_acquire);
            if (a & 1u) continue;
            std::memcpy(buf, c.name, sizeof(buf));
            std::atomic_thread_fence(std::memory_order_acquire);
            if (c.nameSeq.load(std::memory_order_acquire) == a) { buf[sizeof(buf) - 1] = 0; return buf; }
        }
        return std::string();
    }

    // is GENRA CHOP alive? (a non-audio thread asks every few hundred ms)
    struct Alive
    {
        uint32_t last = 0;
        bool ever = false;
        long long seenMs = 0;
        bool check(uint32_t beat, long long nowMs)
        {
            if (beat != last) { last = beat; seenMs = nowMs; ever = true; }
            return ever && nowMs - seenMs < 1500;
        }
    };

private:
    LinkData* data_ = nullptr;
    std::string path_, err_;
};

} // namespace genralink

#endif
