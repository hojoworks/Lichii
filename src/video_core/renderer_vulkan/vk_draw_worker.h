// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <thread>

#include "common/common_types.h"

namespace Vulkan {

/// EDEN_NX2_PARALLEL_DRAW: one persistent worker for a draw's buffer-cache half. The back
/// submits a job, runs its own texture half, then joins. The worker never submits Vulkan
/// commands or records with the Scheduler; it only resolves buffer bindings, fills the shared
/// descriptor capture and defers its scheduler writes for the back to replay.
///
/// The handoff is a spin sequence rather than a condition variable: a draw's buffer half is a
/// few microseconds, and a scheduler wake cost more than the work itself.
class DrawWorker final {
public:
    static DrawWorker& Get();

    /// Starts `run(argument)` on the worker. One job in flight: the back joins it first.
    void Run(void (*run)(void*), void* argument);
    /// Waits for the submitted job to finish.
    void Wait();

    DrawWorker(const DrawWorker&) = delete;
    DrawWorker& operator=(const DrawWorker&) = delete;

private:
    DrawWorker();
    ~DrawWorker();

    void Loop();

    std::atomic<u64> request{0};
    std::atomic<u64> completed{0};
    void (*run)(void*) = nullptr;
    void* argument = nullptr;
    std::atomic<bool> stop{false};
    std::thread thread;
};

} // namespace Vulkan
