#include "core/jobs.h"

namespace zl {

namespace {
thread_local bool t_in_job = false;
}

bool JobSystem::in_job() { return t_in_job; }

JobSystem::JobSystem(int threads) {
    if (threads <= 0) threads = static_cast<int>(std::thread::hardware_concurrency());
    if (threads < 1) threads = 1;
    workers_.reserve(static_cast<size_t>(threads - 1));
    for (int i = 0; i < threads - 1; ++i) workers_.emplace_back([this] { worker_main(); });
}

JobSystem::~JobSystem() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    for (std::thread& t : workers_) t.join();
}

void JobSystem::drain() {
    t_in_job = true;
    for (;;) {
        const i32 begin = next_.fetch_add(grain_, std::memory_order_relaxed);
        if (begin >= count_) break;
        const i32 end = begin + grain_ < count_ ? begin + grain_ : count_;
        fn_(ctx_, begin, end);
    }
    t_in_job = false;
}

void JobSystem::run(i32 count, i32 grain, ChunkFn fn, void* ctx) {
    {
        std::unique_lock<std::mutex> lock(mutex_);
        // A worker that woke late for the previous job may still be reading it.
        idle_.wait(lock, [this] { return active_ == 0; });
        fn_ = fn;
        ctx_ = ctx;
        count_ = count;
        grain_ = grain;
        next_.store(0, std::memory_order_relaxed);
        ++generation_;
    }
    wake_.notify_all();

    drain();

    // Every chunk has been claimed; wait for the ones still running on workers.
    std::unique_lock<std::mutex> lock(mutex_);
    idle_.wait(lock, [this] { return active_ == 0; });
}

void JobSystem::worker_main() {
    u64 seen = 0;
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        wake_.wait(lock, [&] { return stop_ || generation_ != seen; });
        if (stop_) return;
        seen = generation_;
        ++active_;
        lock.unlock();
        drain();
        lock.lock();
        if (--active_ == 0) idle_.notify_all();
    }
}

}  // namespace zl
