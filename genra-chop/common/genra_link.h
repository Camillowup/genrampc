// genra_link.h - LINK v2: the shared memory between GENRA CHOP (the chef) and its companion plugins
// (GENRA BASS today; drums, melody and pads later), each on its own MPC track.
//
// SPDX-License-Identifier: GPL-3.0-only
//
// All plugins run inside the MPC application and meet in one small file mapped into memory (mmap, MAP_SHARED):
// /dev/shm/genra-link-v2, or /tmp/genra-link-v2. Only open/ftruncate/mmap from libc are used (no librt).
//
// GENRA CHOP writes the key, the chords, the style and the rendered loop (4 bars of 16 steps) of every track.
// The loop is written by GENRA CHOP's worker thread under a sequence counter (seqlock); readers copy it in a
// non-audio thread and hand it to their audio thread. Everything else is a lock-free atomic.
//
// A companion has a slot per role (LANE_DRUMS .. LANE_PADS): it beats there (alive), writes its preset name and
// bumps newRequest when its own NEW is pressed; GENRA CHOP then rolls that track again.
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

static const uint32_t kMagic = 0x474E4C32u; // "GNL2"
static const uint32_t kVersion = 2;
static const int kChops = 64;
static const int kSteps = 64;   // 4 bars of 16
static const int kBars = 4;
enum Lane { LANE_DRUMS = 0, LANE_BASS, LANE_MEL, LANE_PADS, NUM_LANES };

// one note of a tone track (bass, melody), one step
struct StepNote
{
    int8_t  pitch;  // MIDI note, -1: none
    uint8_t len16;  // length in 16ths of a step (16 = one step)
    uint8_t vel;    // 1..127
    uint8_t flags;  // 1 accent, 2 slide
};

// the loop, copied as a whole under the seqlock
struct Loop
{
    StepNote bass[kSteps];
    StepNote mel[kSteps];
    int8_t   pad[kSteps][4];   // pad chord voices, -1: none (new chord starts here)
    uint8_t  padLen[kSteps];   // in steps
    uint8_t  drums[kSteps];    // bit per drum voice (GENRA order: kick, snare, clap, rim, ch, oh, tom, perc)
    int8_t   barRoot[kBars], barQuality[kBars];
    uint16_t swingX1000;       // 500 = straight
    int16_t  style;
};

struct Companion
{
    std::atomic<uint32_t> beat;        // +1 every audio block: this companion is alive
    std::atomic<uint32_t> newRequest;  // +1 when its own NEW was pressed
    std::atomic<uint32_t> nameSeq;     // seqlock for name
    char name[24];                     // its preset / sound ("Upright")
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
    std::atomic<uint32_t> loopSeq;         // seqlock for loop: odd while writing; changes = a new loop
    Loop                  loop;
    // written by the companions
    Companion             lane[NUM_LANES];
};

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
        const char* candidates[] = {"/dev/shm/genra-link-v2", "/tmp/genra-link-v2"};
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
                std::memset(&data_->loop, 0xff, sizeof(Loop)); // all notes -1
                data_->loop.swingX1000 = 500;
                data_->version.store(kVersion);
                data_->magic.store(kMagic);
            }
            return true;
        }
        return false;
#endif
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
    // a companion's non-audio thread: copy the loop; false if none could be read (being written, or none yet)
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

private:
    LinkData* data_ = nullptr;
    std::string path_, err_;
};

} // namespace genralink

#endif
