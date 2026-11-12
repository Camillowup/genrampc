// chop_link.h - the shared memory between CHOP TEST (writes) and CHOP LINK (reads).
//
// SPDX-License-Identifier: GPL-3.0-only
//
// Both plugins run inside the MPC application. They meet in one small file that is mapped into
// memory (mmap, MAP_SHARED): /dev/shm/genra-chop-link-v1, or /tmp/... if /dev/shm is missing.
// Only open/ftruncate/mmap from libc are used (no librt), so the plugins load on every firmware.
//
// Everything in LinkData is a lock-free atomic: the audio threads read and write it without locks.
// The sample name is written by CHOP TEST's worker thread under a sequence counter (seqlock).
//
// Open the link from the constructor / prepare() / a worker, never from process().

#ifndef GENRA_CHOP_LINK_H
#define GENRA_CHOP_LINK_H

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

namespace choplink {

static const uint32_t kMagic = 0x474E4C4Bu; // "GNLK"
static const uint32_t kVersion = 1;
static const int kPads = 64;

struct LinkData
{
    std::atomic<uint32_t> magic;
    std::atomic<uint32_t> version;
    // written by CHOP TEST
    std::atomic<uint32_t> writerBeat;      // +1 every audio block: CHOP TEST is alive
    std::atomic<int32_t>  keyRoot;         // 0 = C .. 11, -1 = no key
    std::atomic<int32_t>  keyMinor;        // 1 = minor
    std::atomic<int32_t>  chopCount;       // 0 = no sample
    std::atomic<int32_t>  lastPad;         // 0..63, -1 = none
    std::atomic<uint32_t> trigCount;       // +1 on every pad hit (CHOP LINK follows the hits with it)
    std::atomic<int32_t>  bpmX100;         // the MPC's tempo as CHOP TEST sees it, x100
    std::atomic<int8_t>   chordRoot[kPads];// per pad: 0..11, -1 = no chord (a hit)
    std::atomic<int8_t>   chordQuality[kPads]; // hh::Quality, -1 = none
    std::atomic<uint32_t> nameSeq;         // seqlock for name: odd while writing
    char                  name[48];        // the sample's name
    // written by CHOP LINK
    std::atomic<uint32_t> readerBeat;      // +1 every audio block of any CHOP LINK
};

class Link
{
public:
    Link() = default;
    ~Link() { close(); }
    Link(const Link&) = delete;
    Link& operator=(const Link&) = delete;

    // Returns true if the shared memory could be opened (or created). Where it lives: path().
    bool open()
    {
#if defined(_WIN32)
        return false;
#else
        if (data_) return true;
        const char* candidates[] = {"/dev/shm/genra-chop-link-v1", "/tmp/genra-chop-link-v1"};
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
                // fresh file (all zero) or an old layout: start clean
                data_->keyRoot.store(-1);
                data_->lastPad.store(-1);
                for (int i = 0; i < kPads; ++i) { data_->chordRoot[i].store(-1); data_->chordQuality[i].store(-1); }
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

    // seqlock helpers for the name (writer: one worker thread; readers: any non-audio thread)
    void writeName(const std::string& s)
    {
        if (!data_) return;
        data_->nameSeq.fetch_add(1, std::memory_order_acq_rel); // odd
        std::memset(data_->name, 0, sizeof(data_->name));
        std::strncpy(data_->name, s.c_str(), sizeof(data_->name) - 1);
        data_->nameSeq.fetch_add(1, std::memory_order_acq_rel); // even
    }
    std::string readName() const
    {
        if (!data_) return std::string();
        char buf[sizeof(LinkData::name)];
        for (int tries = 0; tries < 8; ++tries)
        {
            uint32_t a = data_->nameSeq.load(std::memory_order_acquire);
            if (a & 1u) continue;
            std::memcpy(buf, data_->name, sizeof(buf));
            std::atomic_thread_fence(std::memory_order_acquire);
            if (data_->nameSeq.load(std::memory_order_acquire) == a) { buf[sizeof(buf) - 1] = 0; return buf; }
        }
        return std::string();
    }

private:
    LinkData* data_ = nullptr;
    std::string path_, err_;
};

} // namespace choplink

#endif
