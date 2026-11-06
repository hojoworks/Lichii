// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <chrono>
#include <cstdlib>
#include <mutex>
#include <unordered_map>

#include "common/cityhash.h"
#include "common/common_types.h"
#include "common/logging.h"

// EDEN_NX2_CMD_PROBE=1: how often the guest re-submits command lists whose words are
// identical to words submitted before. A command replay cache can only help if this is high.
// Probe only, off unless the variable is set; it reads and hashes each entry at submit time.
namespace VideoCommon::Nx2CmdProbe {

inline bool Enabled() {
    static const bool enabled = std::getenv("EDEN_NX2_CMD_PROBE") != nullptr;
    return enabled;
}

struct State {
    std::mutex mutex;
    u64 window{};
    std::chrono::steady_clock::time_point window_start{};
    // Current window.
    u64 lists{};
    u64 bytes{};
    u64 unique{};
    u64 repeat_prev_frame{};
    u64 repeat_same_frame{};
    u64 same_addr_same{};
    u64 moved_same{};
    u64 same_addr_diff{};
    u64 seq_total{};
    u64 seq_repeat_prev{};
    // Submission being assembled.
    u64 seq_hash{};
    u32 seq_entries{};
    // Hash -> time last seen (ns). Address -> last hash. Sequence hash -> time last seen.
    std::unordered_map<u64, u64> hash_last_ns;
    std::unordered_map<u64, u64> addr_hash;
    std::unordered_map<u64, u64> seq_last_ns;
};

inline State& GetState() {
    static State state;
    return state;
}

inline u64 HashWords(const u32* words, size_t count) {
    return Common::CityHash64(reinterpret_cast<const char*>(words), count * sizeof(u32));
}

inline void ReportLocked(State& s, std::chrono::steady_clock::time_point now) {
    const double seconds =
        std::chrono::duration<double>(now - s.window_start).count();
    const double mib = static_cast<double>(s.bytes) / (1024.0 * 1024.0);
    const auto percent = [](u64 part, u64 total) {
        return total != 0 ? 100.0 * static_cast<double>(part) / static_cast<double>(total) : 0.0;
    };
    LOG_INFO(HW_GPU,
             "nx2 cmd probe {:.1f}s: {} lists ({:.1f} MiB) | unique {} ({:.1f}%) | repeat from a "
             "previous frame {} ({:.1f}%), same frame {} ({:.1f}%) | same-address same-hash {} "
             "({:.1f}%), moved same-hash {} ({:.1f}%), same-address other-hash {} ({:.1f}%) | "
             "submissions {}, repeated from an earlier frame {} ({:.1f}%)",
             seconds, s.lists, mib, s.unique, percent(s.unique, s.lists), s.repeat_prev_frame,
             percent(s.repeat_prev_frame, s.lists), s.repeat_same_frame,
             percent(s.repeat_same_frame, s.lists), s.same_addr_same,
             percent(s.same_addr_same, s.lists), s.moved_same, percent(s.moved_same, s.lists),
             s.same_addr_diff, percent(s.same_addr_diff, s.lists), s.seq_total,
             s.seq_repeat_prev, percent(s.seq_repeat_prev, s.seq_total));
    ++s.window;
    s.lists = 0;
    s.bytes = 0;
    s.unique = 0;
    s.repeat_prev_frame = 0;
    s.repeat_same_frame = 0;
    s.same_addr_same = 0;
    s.moved_same = 0;
    s.same_addr_diff = 0;
    s.seq_total = 0;
    s.seq_repeat_prev = 0;
}

inline void BeginSubmission() {
    if (!Enabled()) {
        return;
    }
    auto& s = GetState();
    std::scoped_lock lock{s.mutex};
    s.seq_hash = 0xcbf29ce484222325ULL;
    s.seq_entries = 0;
}

inline void Observe(const u32* words, size_t count, GPUVAddr address) {
    if (!Enabled() || count == 0) {
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

    const u64 hash = HashWords(words, count);
    ++s.lists;
    s.bytes += count * sizeof(u32);

    const u64 now_ns = static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count());
    constexpr u64 FrameNs = 16'666'667;

    const auto hash_it = s.hash_last_ns.find(hash);
    const bool hash_seen = hash_it != s.hash_last_ns.end();
    if (!hash_seen) {
        ++s.unique;
    } else if (now_ns - hash_it->second >= FrameNs) {
        ++s.repeat_prev_frame;
    } else {
        ++s.repeat_same_frame;
    }

    const u64 addr = static_cast<u64>(address);
    const auto addr_it = s.addr_hash.find(addr);
    if (addr_it == s.addr_hash.end()) {
        if (hash_seen) {
            ++s.moved_same;
        }
    } else if (addr_it->second == hash) {
        ++s.same_addr_same;
    } else {
        ++s.same_addr_diff;
    }
    s.addr_hash[addr] = hash;
    s.hash_last_ns[hash] = now_ns;

    s.seq_hash = (s.seq_hash ^ hash) * 0x100000001b3ULL;
    ++s.seq_entries;

    // Keep the probe bounded on pathological streams.
    if (s.hash_last_ns.size() > 2'000'000) {
        s.hash_last_ns.clear();
    }
    if (s.addr_hash.size() > 1'000'000) {
        s.addr_hash.clear();
    }
}

inline void EndSubmission() {
    if (!Enabled()) {
        return;
    }
    auto& s = GetState();
    std::scoped_lock lock{s.mutex};
    if (s.seq_entries == 0) {
        return;
    }
    const u64 now_ns = static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    constexpr u64 FrameNs = 16'666'667;
    ++s.seq_total;
    const auto it = s.seq_last_ns.find(s.seq_hash);
    if (it != s.seq_last_ns.end() && now_ns - it->second >= FrameNs) {
        ++s.seq_repeat_prev;
    }
    s.seq_last_ns[s.seq_hash] = now_ns;
    if (s.seq_last_ns.size() > 200'000) {
        s.seq_last_ns.clear();
    }
}

} // namespace VideoCommon::Nx2CmdProbe
