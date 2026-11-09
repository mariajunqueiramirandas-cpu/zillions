// A minimal fork-join job system: one persistent pool of worker threads and a
// blocking parallel_for. Work is split into chunks that threads claim with a
// single atomic add, so there are no per-chunk allocations or locks.
#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <type_traits>
#include <vector>

#include "core/base.h"

namespace zl {

class JobSystem {
public:
    // threads == 0 picks the hardware concurrency. The calling thread always
    // participates, so `threads` counts it as well.
    explicit JobSystem(int threads = 0);
    ~JobSystem();

    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;

    int thread_count() const { return static_cast<int>(workers_.size()) + 1; }

    // Calls fn(begin, end) over [0, count) in chunks of at most `grain`, in
    // parallel, and returns once every chunk has finished. Chunks must not
    // depend on each other. Nested calls run inline on the calling thread.
    template <class F>
    void parallel_for(i32 count, i32 grain, F&& fn) {
        if (count <= 0) return;
        if (grain < 1) grain = 1;
        if (count <= grain || workers_.empty() || in_job()) {
            fn(0, count);
            return;
        }
        auto thunk = [](void* ctx, i32 begin, i32 end) { (*static_cast<std::remove_reference_t<F>*>(ctx))(begin, end); };
        run(count, grain, thunk, &fn);
    }

private:
    using ChunkFn = void (*)(void*, i32, i32);

    static bool in_job();
    void run(i32 count, i32 grain, ChunkFn fn, void* ctx);
    void drain();
    void worker_main();

    std::vector<std::thread> workers_;

    std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable idle_;
    u64 generation_ = 0;
    int active_ = 0;
    bool stop_ = false;

    // Current job. Written only while no worker is active.
    ChunkFn fn_ = nullptr;
    void* ctx_ = nullptr;
    i32 count_ = 0;
    i32 grain_ = 1;
    std::atomic<i32> next_{0};
};

}  // namespace zl
