// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_core/renderer_vulkan/vk_draw_worker.h"

#include <thread>

#include "common/thread.h"

namespace Vulkan {

namespace {

inline void Pause() {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    _mm_pause();
#elif defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    asm volatile("yield");
#else
    std::this_thread::yield();
#endif
}

} // Anonymous namespace

DrawWorker& DrawWorker::Get() {
    static DrawWorker worker;
    return worker;
}

DrawWorker::DrawWorker() {
    thread = std::thread([this] { Loop(); });
}

DrawWorker::~DrawWorker() {
    stop.store(true, std::memory_order_release);
    if (thread.joinable()) {
        thread.join();
    }
}

void DrawWorker::Run(void (*run_)(void*), void* argument_) {
    // The back joins every job before the next draw: one in flight.
    run = run_;
    argument = argument_;
    request.fetch_add(1, std::memory_order_release);
}

void DrawWorker::Wait() {
    const u64 target = request.load(std::memory_order_relaxed);
    u32 spins = 0;
    while (completed.load(std::memory_order_acquire) != target) {
        if (++spins < 4000) {
            Pause();
        } else {
            spins = 0;
            std::this_thread::yield();
        }
    }
}

void DrawWorker::Loop() {
    Common::SetCurrentThreadName("GPUDrawResolve");
    Common::SetCurrentThreadPriority(Common::ThreadPriority::High);
    Common::SetCurrentThreadToPerformanceCores();

    u64 seen = 0;
    while (!stop.load(std::memory_order_acquire)) {
        const u64 req = request.load(std::memory_order_acquire);
        if (req == seen) {
            // Spin briefly so a burst of draws never pays a scheduler wake, then yield the core.
            u32 spins = 0;
            while (request.load(std::memory_order_relaxed) == seen &&
                   !stop.load(std::memory_order_relaxed) && ++spins < 20000) {
                Pause();
            }
            if (request.load(std::memory_order_relaxed) == seen) {
                std::this_thread::yield();
            }
            continue;
        }
        void (*job)(void*) = run;
        void* job_argument = argument;
        job(job_argument);
        seen = req;
        completed.store(seen, std::memory_order_release);
    }
}

} // namespace Vulkan
