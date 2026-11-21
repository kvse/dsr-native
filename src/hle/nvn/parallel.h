#pragma once
// A tiny fork-join pool for CPU-heavy work on the render thread (BC -> ETC2/RGBA8 texture conversion).
// From pull request #13 (fernanfez00).
// Run() returns once every task has run; the calling thread helps. Workers are not pinned to the cores the
// caller may be pinned to: they may use every CPU, so the efficiency cores take part too.
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

#if defined(__linux__)
#include <sched.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace dsr::hle::nvn {

class WorkPool {
public:
    static WorkPool& Get() {
        static WorkPool* p = new WorkPool();  // never destroyed: workers are detached
        return *p;
    }

    // Runs fn(0) .. fn(count - 1) on the pool and the calling thread; returns when all are done.
    void Run(size_t count, const std::function<void(size_t)>& fn) {
        if (count == 0) return;
        std::lock_guard<std::mutex> one_at_a_time(run_m_);
        EnsureStarted();
        if (count == 1 || workers_ == 0) {
            for (size_t i = 0; i < count; i++) fn(i);
            return;
        }
        auto b = std::make_shared<Batch>();
        b->fn = &fn;
        b->count = count;
        {
            std::lock_guard<std::mutex> lk(m_);
            batch_ = b;
            gen_++;
        }
        cv_.notify_all();
        Help(*b);
        std::unique_lock<std::mutex> lk(done_m_);
        done_cv_.wait(lk, [&] { return b->done.load(std::memory_order_acquire) == b->count; });
    }

    int Workers() {
        std::lock_guard<std::mutex> lk(run_m_);
        EnsureStarted();
        return workers_;
    }

private:
    struct Batch {
        const std::function<void(size_t)>* fn = nullptr;
        size_t count = 0;
        std::atomic<size_t> next{0};
        std::atomic<size_t> done{0};
    };

    void Help(Batch& b) {
        for (;;) {
            const size_t i = b.next.fetch_add(1, std::memory_order_relaxed);
            if (i >= b.count) break;
            (*b.fn)(i);
            if (b.done.fetch_add(1, std::memory_order_acq_rel) + 1 == b.count) {
                std::lock_guard<std::mutex> lk(done_m_);
                done_cv_.notify_all();
            }
        }
    }

    void EnsureStarted() {
        if (started_) return;
        started_ = true;
        const unsigned hw = std::thread::hardware_concurrency();
        workers_ = hw > 1 ? int(std::min(hw - 1, 6u)) : 0;
#ifdef DSR_POOL_FORCE_WORKERS  // tests only
        workers_ = DSR_POOL_FORCE_WORKERS;
#endif
        for (int i = 0; i < workers_; i++) std::thread([this] { WorkerMain(); }).detach();
    }

    void WorkerMain() {
#if defined(__linux__)
        cpu_set_t all;
        CPU_ZERO(&all);
        const long n = sysconf(_SC_NPROCESSORS_CONF);
        for (long c = 0; c < n && c < CPU_SETSIZE; c++) CPU_SET(c, &all);
        sched_setaffinity(0, sizeof(all), &all);  // best effort
        // Texture conversion bursts (hundreds of BC levels when a zone loads) must not take the fast cores from the
        // game's main thread and the render thread: on a 2 fast + 6 slow chip the logs showed multi-second stalls
        // (render thread running but starved, 0 ms waiting for the GPU) exactly while those bursts ran. Nice 10 on
        // Linux is per thread: the workers still use idle cores at full speed.
        setpriority(PRIO_PROCESS, 0, 10);
#endif
        uint64_t seen = 0;
        for (;;) {
            std::shared_ptr<Batch> b;
            {
                std::unique_lock<std::mutex> lk(m_);
                cv_.wait(lk, [&] { return gen_ != seen; });
                seen = gen_;
                b = batch_;
            }
            if (b) Help(*b);
        }
    }

    std::mutex run_m_;
    std::mutex m_;
    std::condition_variable cv_;
    std::mutex done_m_;
    std::condition_variable done_cv_;
    std::shared_ptr<Batch> batch_;
    uint64_t gen_ = 0;
    bool started_ = false;
    int workers_ = 0;
};

}  // namespace dsr::hle::nvn
