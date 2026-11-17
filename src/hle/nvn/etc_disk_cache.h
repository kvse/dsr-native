#pragma once
// Persistent store for BC -> ETC2 texture conversions (GPUs without BC support: Mali, PowerVR, ...).
//
// Converting the game's BC textures on the CPU is the biggest cost of a first visit to an area (seconds on a
// phone with two fast cores). The in-memory cache in vk_backend.cpp only helps within one run; this keeps the
// converted levels on disk so the next run reads them back instead of encoding them again.
//
// Layout, in the cache directory:
//   etc2_cache.dat  the converted levels, appended one after another;
//   etc2_cache.idx  a 16-byte header, then one 40-byte entry per level (key, tag, offset, size, payload checksum).
// Entries are written to the data file first and to the index second, so a crash can only leave unreferenced
// data behind. Every level read back is checked against its checksum; a bad one is treated as a miss.
//
// Identity of a level: `key` is the cache key vk_backend.cpp already computes (hash of the source blocks plus
// format and size); `tag` is a second, independent hash of the same source blocks (Tag()). A level is only
// returned when both match, so a collision of the cheaper in-memory hash cannot hand back a wrong texture
// across runs.
//
// Writes happen on a background thread (flash is slow); Lookup() is a synchronous pread.
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dsr::hle::nvn {

class EtcDiskCache {
public:
    // Bump when etc_encode.cpp or bc_decode.cpp would produce different bytes for the same input: files written
    // with another revision are discarded when opened.
    static constexpr uint32_t kCodecRevision = 1;

    // The process-wide cache used by the Vulkan backend (never destroyed: its writer thread is joined only by
    // the destructor, which tests use on their own instances).
    static EtcDiskCache& Get();

    EtcDiskCache() = default;
    ~EtcDiskCache();
    EtcDiskCache(const EtcDiskCache&) = delete;
    EtcDiskCache& operator=(const EtcDiskCache&) = delete;

    // Opens (or creates) the cache files in `dir`. `max_bytes` limits the data file; 0 or an empty dir leaves the
    // cache disabled. Only the first call does anything.
    void Open(const std::string& dir, uint64_t max_bytes);
    bool Enabled() const { return enabled_.load(std::memory_order_acquire); }

    // Hash of the source blocks of a level, independent of the one used for the key.
    static uint64_t Tag(const void* blocks, size_t bytes);

    // Copies the stored level into `out` (exactly `bytes` long). False if it is not stored, has another size, or
    // fails its checksum.
    bool Lookup(uint64_t key, uint64_t tag, uint8_t* out, size_t bytes);

    // Queues a freshly converted level for writing on the background thread (the data is copied). Dropped when the
    // cache is full, the queue is long, or an earlier write failed.
    void Store(uint64_t key, uint64_t tag, const uint8_t* data, size_t bytes);

    // Waits until everything accepted by Store() so far has been written (tests; also useful before exit).
    void Flush();

    struct Stats {
        uint64_t entries = 0, data_bytes = 0;
        uint64_t hits = 0, misses = 0, corrupt = 0, stored = 0, dropped = 0;
    };
    Stats GetStats() const;

private:
    struct DiskEntry {
        uint64_t key, tag, offset;
        uint32_t size, pad;
        uint64_t checksum;
    };
    struct Item {
        uint64_t key, tag;
        std::vector<uint8_t> data;
    };

    void LoadIndex();
    void WriterMain();
    void CloseFiles();

    std::mutex open_m_;
    bool opened_ = false;
    std::atomic<bool> enabled_{false};
    std::atomic<bool> writes_ok_{true};

    std::string data_path_, idx_path_;
    int data_fd_ = -1, idx_fd_ = -1;
    uint64_t max_bytes_ = 0;

    mutable std::mutex m_;  // index_, pending_*, data_end_, idx_end_, stop_, writing_
    std::condition_variable cv_, idle_cv_;
    std::unordered_map<uint64_t, DiskEntry> index_;
    std::deque<Item> pending_;
    std::unordered_set<uint64_t> pending_keys_;
    uint64_t pending_bytes_ = 0;
    uint64_t data_end_ = 0, idx_end_ = 0;
    bool stop_ = false, writing_ = false, full_logged_ = false;
    std::thread writer_;

    std::atomic<uint64_t> hits_{0}, misses_{0}, corrupt_{0}, stored_{0}, dropped_{0};
};

}  // namespace dsr::hle::nvn
