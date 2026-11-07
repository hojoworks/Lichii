// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstdlib>

#include "common/common_types.h"

// EDEN_NX2_GPU_DRAWS: the buffer cache records, per draw, the byte offset of each constant
// buffer's slice inside the bound uniform buffer. The shader adds that base to every constant
// access, so one descriptor can serve a whole multi-draw batch.
namespace VideoCommon::Nx2DrawParams {

inline bool Enabled() {
    static const bool enabled = std::getenv("EDEN_NX2_GPU_DRAWS") != nullptr;
    return enabled;
}

/// Matches Shader::Info::MAX_CBUFS.
inline constexpr u32 MaxCbufs = 18;
/// Matches the 64-draw table in GraphicsPipeline's DrawParams buffer.
inline constexpr u32 MaxDraws = 64;

inline thread_local std::array<u32, MaxCbufs> cb_bases{};

/// Set by the pipeline build around the stage emission: only vertex+fragment pipelines can carry
/// the flat DrawID varying.
inline thread_local bool compile_gpu_draws = false;

inline void Reset() {
    cb_bases.fill(0);
}

} // namespace VideoCommon::Nx2DrawParams
