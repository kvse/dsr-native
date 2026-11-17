#include "hle/nvn/etc_disk_cache.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

#include "common/log.h"

namespace dsr::hle::nvn {
namespace {

constexpr const char* kTag = "etccache";
constexpr uint32_t kMagic = 0x45525344;  // "DSRE"
constexpr uint32_t kFormatVersion = 1;
constexpr size_t kHeaderBytes = 16;
constexpr size_t kEntryBytes = 40;
constexpr size_t kMaxLevelBytes = 32u << 20;     // a single converted level larger than this is not stored
constexpr uint64_t kMaxPendingBytes = 64u << 20;  // queued for writing; beyond this new levels are dropped
constexpr uint64_t kMaxIndexBytes = 128u << 20;   // an index file larger than this is considered corrupt
constexpr size_t kBatchBytes = 8u << 20;          // the writer takes about this much per round
constexpr uint64_t kTagSeed = 0x6a09e667f3bcc909ull;
constexpr uint64_t kSumSeed = 0xbb67ae8584caa73bull;

// Two-lane multiply/xor-shift hash with a murmur-style final mix. Not cryptographic: it guards against bit rot,
// torn writes and accidental collisions, not against someone editing the cache.
uint64_t Hash64(const void* data, size_t n, uint64_t seed) {
    const auto* p = static_cast<const uint8_t*>(data);
    uint64_t a = seed ^ (uint64_t(n) * 0x9E3779B97F4A7C15ull), b = ~seed;
    size_t i = 0;
    for (; i + 16 <= n; i += 16) {
        uint64_t x, y;
        memcpy(&x, p + i, 8);
        memcpy(&y, p + i + 8, 8);
        a = (a ^ x) * 0xFF51AFD7ED558CCDull;
        a ^= a >> 29;
        b = (b ^ y) * 0xC4CEB9FE1A85EC53ull;
        b ^= b >> 31;
        a += b;
    }
    if (i + 8 <= n) {  // up to 15 bytes are left: one whole word, then fewer than 8 bytes
        uint64_t x;
        memcpy(&x, p + i, 8);
        a = (a ^ x) * 0xFF51AFD7ED558CCDull;
        a ^= a >> 29;
        i += 8;
    }
    if (i < n) {
        uint64_t tail = 0;
        memcpy(&tail, p + i, n - i);
        b = (b ^ tail) * 0xC4CEB9FE1A85EC53ull;
        b ^= b >> 31;
    }
    uint64_t h = a ^ (b * 0x9E3779B97F4A7C15ull);
    h ^= h >> 33;
    h *= 0xFF51AFD7ED558CCDull;
    h ^= h >> 33;
    h *= 0xC4CEB9FE1A85EC53ull;
    h ^= h >> 33;
    return h;
}

bool PReadAll(int fd, void* buf, size_t n, uint64_t off) {
    auto* p = static_cast<uint8_t*>(buf);
    size_t got = 0;
    while (got < n) {
        const ssize_t r = pread(fd, p + got, n - got, off_t(off + got));
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return false;
        got += size_t(r);
    }
    return true;
}

bool Trunc(int fd, uint64_t n) { return ftruncate(fd, off_t(n)) == 0; }

bool PWriteAll(int fd, const void* buf, size_t n, uint64_t off) {
    const auto* p = static_cast<const uint8_t*>(buf);
    size_t put = 0;
    while (put < n) {
        const ssize_t r = pwrite(fd, p + put, n - put, off_t(off + put));
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return false;
        put += size_t(r);
    }
    return true;
}

}  // namespace

EtcDiskCache& EtcDiskCache::Get() {
    static EtcDiskCache* c = new EtcDiskCache();  // never destroyed
    return *c;
}

EtcDiskCache::~EtcDiskCache() {
    {
        std::lock_guard<std::mutex> lk(m_);
        stop_ = true;
    }
    cv_.notify_all();
    if (writer_.joinable()) writer_.join();
    CloseFiles();
}

void EtcDiskCache::CloseFiles() {
    if (data_fd_ >= 0) close(data_fd_);
    if (idx_fd_ >= 0) close(idx_fd_);
    data_fd_ = idx_fd_ = -1;
}

uint64_t EtcDiskCache::Tag(const void* blocks, size_t bytes) { return Hash64(blocks, bytes, kTagSeed); }

void EtcDiskCache::Open(const std::string& dir, uint64_t max_bytes) {
    std::lock_guard<std::mutex> once(open_m_);
    if (opened_) return;
    opened_ = true;
    if (dir.empty() || max_bytes == 0) {
        LOGI(kTag, "ETC2 disk cache off");
        return;
    }
    max_bytes_ = max_bytes;
    data_path_ = dir + "/etc2_cache.dat";
    idx_path_ = dir + "/etc2_cache.idx";
    data_fd_ = open(data_path_.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0664);
    idx_fd_ = open(idx_path_.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0664);
    if (data_fd_ < 0 || idx_fd_ < 0) {
        LOGW(kTag, "ETC2 disk cache: cannot open files in %s (errno %d): off", dir.c_str(), errno);
        CloseFiles();
        return;
    }
    LoadIndex();
    writer_ = std::thread([this] { WriterMain(); });
    enabled_.store(true, std::memory_order_release);
    LOGI(kTag, "ETC2 disk cache: %zu levels, %.0f MB of %.0f MB used (%s)", index_.size(), data_end_ / 1048576.0,
         max_bytes_ / 1048576.0, dir.c_str());
}

// Reads the index. Anything that does not match this build (format, codec revision) or looks damaged empties
// both files: a cache is only ever worth what it gets back.
void EtcDiskCache::LoadIndex() {
    struct stat ist {}, dst {};
    fstat(idx_fd_, &ist);
    fstat(data_fd_, &dst);
    const uint64_t idx_size = uint64_t(ist.st_size), data_size = uint64_t(dst.st_size);

    bool fresh = true;
    uint32_t hdr[4] = {};
    if (idx_size >= kHeaderBytes && idx_size <= kMaxIndexBytes && PReadAll(idx_fd_, hdr, sizeof(hdr), 0) &&
        hdr[0] == kMagic && hdr[1] == kFormatVersion && hdr[2] == kCodecRevision && hdr[3] == kEntryBytes)
        fresh = false;

    if (fresh) {
        if (idx_size || data_size) LOGI(kTag, "ETC2 disk cache: other version or damaged, starting empty");
        if (!Trunc(data_fd_, 0) || !Trunc(idx_fd_, 0)) writes_ok_ = false;
        const uint32_t h[4] = {kMagic, kFormatVersion, kCodecRevision, uint32_t(kEntryBytes)};
        if (!PWriteAll(idx_fd_, h, sizeof(h), 0)) writes_ok_ = false;
        data_end_ = 0;
        idx_end_ = kHeaderBytes;
        return;
    }

    const size_t n = size_t((idx_size - kHeaderBytes) / kEntryBytes);
    std::vector<DiskEntry> entries(n);
    if (n && !PReadAll(idx_fd_, entries.data(), n * kEntryBytes, kHeaderBytes)) {
        LOGW(kTag, "ETC2 disk cache: index unreadable, starting empty");
        entries.clear();
        if (!Trunc(data_fd_, 0) || !Trunc(idx_fd_, kHeaderBytes)) writes_ok_ = false;
        data_end_ = 0;
        idx_end_ = kHeaderBytes;
        return;
    }
    index_.reserve(n);
    uint64_t dropped = 0;
    for (const DiskEntry& e : entries) {
        if (e.size == 0 || e.size > kMaxLevelBytes || e.offset + e.size > data_size) {
            dropped++;  // data never made it to the file (or an entry from a crash): ignore
            continue;
        }
        index_[e.key] = e;  // a later entry for the same key replaces the earlier one
    }
    idx_end_ = kHeaderBytes + uint64_t(n) * kEntryBytes;
    if (idx_size != idx_end_ && !Trunc(idx_fd_, idx_end_)) writes_ok_ = false;  // drop a half-written last entry
    data_end_ = data_size;
    if (dropped) LOGW(kTag, "ETC2 disk cache: %llu index entries point outside the data file", (unsigned long long)dropped);
}

bool EtcDiskCache::Lookup(uint64_t key, uint64_t tag, uint8_t* out, size_t bytes) {
    if (!Enabled()) return false;
    DiskEntry e;
    {
        std::lock_guard<std::mutex> lk(m_);
        auto it = index_.find(key);
        if (it == index_.end() || it->second.tag != tag || it->second.size != bytes) {
            misses_++;
            return false;
        }
        e = it->second;
    }
    // Read into ordinary memory and verify before touching `out`: the caller's destination may be GPU-visible
    // memory that is slow to read back.
    thread_local std::vector<uint8_t> buf;
    buf.resize(bytes);
    if (!PReadAll(data_fd_, buf.data(), bytes, e.offset) || Hash64(buf.data(), bytes, kSumSeed) != e.checksum) {
        corrupt_++;
        std::lock_guard<std::mutex> lk(m_);
        auto it = index_.find(key);
        if (it != index_.end() && it->second.offset == e.offset) index_.erase(it);
        return false;
    }
    memcpy(out, buf.data(), bytes);
    hits_++;
    return true;
}

void EtcDiskCache::Store(uint64_t key, uint64_t tag, const uint8_t* data, size_t bytes) {
    if (!Enabled() || !writes_ok_.load(std::memory_order_relaxed) || bytes == 0 || bytes > kMaxLevelBytes) return;
    {
        std::lock_guard<std::mutex> lk(m_);
        auto it = index_.find(key);
        if ((it != index_.end() && it->second.tag == tag) || pending_keys_.count(key)) return;  // already there
        if (data_end_ + pending_bytes_ + bytes > max_bytes_) {
            if (!full_logged_) {
                full_logged_ = true;
                LOGI(kTag, "ETC2 disk cache full (%.0f MB): new levels are not stored (delete %s to start over)",
                     max_bytes_ / 1048576.0, data_path_.c_str());
            }
            dropped_++;
            return;
        }
        if (pending_bytes_ + bytes > kMaxPendingBytes) {
            dropped_++;
            return;
        }
        pending_keys_.insert(key);
        pending_bytes_ += bytes;
    }
    Item item{key, tag, std::vector<uint8_t>(data, data + bytes)};  // copied outside the lock
    {
        std::lock_guard<std::mutex> lk(m_);
        pending_.push_back(std::move(item));
    }
    cv_.notify_one();
}

void EtcDiskCache::WriterMain() {
    std::unique_lock<std::mutex> lk(m_);
    for (;;) {
        cv_.wait(lk, [&] { return stop_ || !pending_.empty(); });
        if (pending_.empty()) {
            if (stop_) break;
            continue;
        }
        if (!writes_ok_.load()) {  // an earlier write failed: discard
            pending_.clear();
            pending_keys_.clear();
            pending_bytes_ = 0;
            idle_cv_.notify_all();
            continue;
        }
        std::vector<Item> batch;
        size_t batch_bytes = 0;
        while (!pending_.empty() && (batch.empty() || batch_bytes < kBatchBytes)) {
            batch_bytes += pending_.front().data.size();
            batch.push_back(std::move(pending_.front()));
            pending_.pop_front();
        }
        writing_ = true;
        uint64_t off = data_end_;
        const uint64_t idx_off = idx_end_;
        lk.unlock();

        std::vector<DiskEntry> entries;
        entries.reserve(batch.size());
        bool ok = true;
        for (const Item& it : batch) {
            if (!PWriteAll(data_fd_, it.data.data(), it.data.size(), off)) {
                ok = false;
                break;
            }
            entries.push_back({it.key, it.tag, off, uint32_t(it.data.size()), 0,
                               Hash64(it.data.data(), it.data.size(), kSumSeed)});
            off += it.data.size();
        }
        // Index after data: an entry never points at bytes that are not in the file.
        if (ok && !PWriteAll(idx_fd_, entries.data(), entries.size() * kEntryBytes, idx_off)) ok = false;

        lk.lock();
        if (ok) {
            for (const DiskEntry& e : entries) index_[e.key] = e;
            data_end_ = off;
            idx_end_ = idx_off + uint64_t(entries.size()) * kEntryBytes;
            stored_ += entries.size();
        } else {
            writes_ok_.store(false);
            LOGW(kTag, "ETC2 disk cache: write failed (errno %d): no more levels are stored this run", errno);
            Trunc(data_fd_, data_end_);  // best effort: back to the last good state
            Trunc(idx_fd_, idx_end_);
        }
        for (const Item& it : batch) {
            pending_keys_.erase(it.key);
            pending_bytes_ -= it.data.size();
        }
        writing_ = false;
        idle_cv_.notify_all();
    }
}

void EtcDiskCache::Flush() {
    if (!Enabled()) return;
    std::unique_lock<std::mutex> lk(m_);
    idle_cv_.wait(lk, [&] { return pending_keys_.empty() && !writing_; });  // keys: also those not queued yet
}

EtcDiskCache::Stats EtcDiskCache::GetStats() const {
    Stats s;
    {
        std::lock_guard<std::mutex> lk(m_);
        s.entries = index_.size();
        s.data_bytes = data_end_;
    }
    s.hits = hits_;
    s.misses = misses_;
    s.corrupt = corrupt_;
    s.stored = stored_;
    s.dropped = dropped_;
    return s;
}

}  // namespace dsr::hle::nvn
