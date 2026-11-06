// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <unordered_map>

#include "common/common_types.h"
#include "common/logging.h"

// EDEN_NX2_DRAW_PROBE=1: how often a draw's resolved state recurs. The draw-state cache and
// multi-draw batching only pay if this is high. Probe only, off unless the variable is set.
namespace VideoCommon::Nx2DrawProbe {

inline bool Enabled() {
    static const bool enabled = std::getenv("EDEN_NX2_DRAW_PROBE") != nullptr;
    return enabled;
}

inline u64 HashFields(const u64* fields, size_t count) {
    u64 hash = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < count; ++i) {
        hash = (hash ^ fields[i]) * 0x100000001b3ULL;
    }
    return hash;
}

struct State {
    std::mutex mutex;
    u64 window{};
    std::chrono::steady_clock::time_point window_start{};
    u64 draws{};
    u64 unique{};
    u64 repeat_prev_frame{};
    u64 repeat_same_frame{};
    u64 distinct{};
    u64 consecutive_same{};
    u64 same_pipeline_consecutive{};
    u64 runs{};
    u64 run_draws{};
    u64 prev_hash{};
    u64 prev_pipeline{};
    u64 current_run{};
    std::unordered_map<u64, u64> hash_last_ns;
    std::unordered_map<u64, u64> pipeline_of_hash;
};

inline State& GetState() {
    static State state;
    return state;
}

inline void ReportLocked(State& s, std::chrono::steady_clock::time_point now) {
    const double seconds = std::chrono::duration<double>(now - s.window_start).count();
    const auto percent = [](u64 part, u64 total) {
        return total != 0 ? 100.0 * static_cast<double>(part) / static_cast<double>(total) : 0.0;
    };
    LOG_INFO(HW_GPU,
             "nx2 draw probe {:.1f}s: {} draws ({:.0f}/s) | distinct state {} | new {} ({:.1f}%) | "
             "repeat from a previous frame {} ({:.1f}%), same frame {} ({:.1f}%) | consecutive "
             "same-state {} ({:.1f}%), {} runs averaging {:.1f} | consecutive same-pipeline "
             "{} ({:.1f}%)",
             seconds, s.draws, static_cast<double>(s.draws) / seconds, s.distinct, s.unique,
             percent(s.unique, s.draws), s.repeat_prev_frame, percent(s.repeat_prev_frame, s.draws),
             s.repeat_same_frame, percent(s.repeat_same_frame, s.draws), s.consecutive_same,
             percent(s.consecutive_same, s.draws), s.runs,
             s.runs != 0 ? static_cast<double>(s.run_draws) / static_cast<double>(s.runs) : 0.0,
             s.same_pipeline_consecutive, percent(s.same_pipeline_consecutive, s.draws));
    ++s.window;
    s.draws = 0;
    s.unique = 0;
    s.repeat_prev_frame = 0;
    s.repeat_same_frame = 0;
    s.distinct = 0;
    s.consecutive_same = 0;
    s.same_pipeline_consecutive = 0;
    s.runs = 0;
    s.run_draws = 0;
}

inline void Observe(u64 hash, u64 pipeline) {
    if (!Enabled()) {
        return;
    }
    auto& s = GetState();
    std::scoped_lock lock{s.mutex};
    const auto now = std::chrono::steady_clock::now();
    if (s.window_start == std::chrono::steady_clock::time_point{}) {
        s.window_start = now;
    } else if (now - s.window_start >= std::chrono::seconds(2)) {
        ReportLocked(s, now);
        s.window_start = now;
    }
    ++s.draws;
    if (hash == s.prev_hash) {
        ++s.consecutive_same;
        ++s.current_run;
    } else {
        if (s.current_run > 1) {
            ++s.runs;
            s.run_draws += s.current_run;
        }
        s.current_run = 1;
        if (pipeline == s.prev_pipeline) {
            ++s.same_pipeline_consecutive;
        }
    }
    s.prev_hash = hash;
    s.prev_pipeline = pipeline;
    const u64 now_ns = static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count());
    constexpr u64 FrameNs = 16'666'667;
    const auto it = s.hash_last_ns.find(hash);
    if (it == s.hash_last_ns.end()) {
        ++s.unique;
        ++s.distinct;
        s.pipeline_of_hash[hash] = pipeline;
    } else if (now_ns - it->second >= FrameNs) {
        ++s.repeat_prev_frame;
    } else {
        ++s.repeat_same_frame;
    }
    s.hash_last_ns[hash] = now_ns;
    if (s.hash_last_ns.size() > 2'000'000) {
        s.hash_last_ns.clear();
        s.pipeline_of_hash.clear();
    }
}

} // namespace VideoCommon::Nx2DrawProbe
