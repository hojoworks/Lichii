// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <mutex>

namespace Vulkan {

// The VMA allocator is created with VMA_ALLOCATOR_CREATE_EXTERNALLY_SYNCHRONIZED_BIT: the
// emulator owns its synchronization. Allocation and destruction normally happen on one thread,
// but EDEN_NX2_PARALLEL_DRAW resolves a draw's buffer half on DrawWorker while the back resolves
// the texture half, and both allocate (staging buffers, images). Every VMA entry point takes this
// lock. Recursive because CreateBuffer may reach CreateBufferInSeparatePool.
inline std::recursive_mutex& VmaMutex() {
    static std::recursive_mutex mutex;
    return mutex;
}

} // namespace Vulkan
