// Monotonic timing helpers.
#pragma once

#include <chrono>

#include "core/base.h"

namespace zl {

inline f64 now_seconds() {
    using clock = std::chrono::steady_clock;
    static const clock::time_point origin = clock::now();
    return std::chrono::duration<f64>(clock::now() - origin).count();
}

class Stopwatch {
public:
    Stopwatch() : start_(now_seconds()) {}
    void reset() { start_ = now_seconds(); }
    f64 seconds() const { return now_seconds() - start_; }
    f64 millis() const { return seconds() * 1000.0; }

private:
    f64 start_;
};

// Drives a fixed-rate simulation from a variable-rate render loop. The
// simulation always advances in identical steps, which keeps it deterministic
// regardless of frame rate; the leftover fraction is exposed for interpolation.
class FixedStep {
public:
    explicit FixedStep(f64 hz) : step_(1.0 / hz) {}

    // Feeds the elapsed frame time and returns how many simulation steps to run.
    int advance(f64 frame_seconds) {
        // Clamp so a long stall (debugger, window drag) cannot cause a step avalanche.
        if (frame_seconds > 0.25) frame_seconds = 0.25;
        accumulator_ += frame_seconds;
        int steps = 0;
        while (accumulator_ >= step_) {
            accumulator_ -= step_;
            ++steps;
        }
        return steps;
    }

    f64 step_seconds() const { return step_; }
    f64 alpha() const { return accumulator_ / step_; }

private:
    f64 step_;
    f64 accumulator_ = 0.0;
};

}  // namespace zl
