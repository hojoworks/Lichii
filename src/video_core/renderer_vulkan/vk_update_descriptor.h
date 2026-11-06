// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <variant>
#include <vector>
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class Device;
class Scheduler;

struct DescriptorAddress {
    VkDeviceAddress address;
    VkDeviceSize range;
    VkFormat format;
};

union DescriptorUpdateEntry {
    DescriptorUpdateEntry() = default;
    DescriptorUpdateEntry(VkDescriptorImageInfo image_) : image{image_} {}
    DescriptorUpdateEntry(VkDescriptorBufferInfo buffer_) : buffer{buffer_} {}
    DescriptorUpdateEntry(VkBufferView texel_buffer_) : texel_buffer{texel_buffer_} {}
    DescriptorUpdateEntry(DescriptorAddress address_) : address{address_} {}
    std::monostate empty{};
    VkDescriptorImageInfo image;
    VkDescriptorBufferInfo buffer;
    VkBufferView texel_buffer;
    DescriptorAddress address;
};

// EDEN_NX2_PARALLEL_DRAW: the buffer half of a draw resolves on a worker thread and records its
// descriptor entries here instead of the real queue; the back appends them, in order, after the
// join. The worker sets the target for its own thread; the back reads the completed storage.
inline std::vector<DescriptorUpdateEntry>& CapturedStorage() {
    static std::vector<DescriptorUpdateEntry> entries;
    return entries;
}
inline thread_local std::vector<DescriptorUpdateEntry>* t_capture_target = nullptr;

class UpdateDescriptorQueue final {
    // This should be plenty for the vast majority of cases. Most desktop platforms only
    // provide up to 3 swapchain images.
    static constexpr size_t FRAMES_IN_FLIGHT = 8;

public:
    static constexpr size_t GUEST_FRAME_PAYLOAD_SIZE = 0x80000;
    static constexpr size_t COMPUTE_FRAME_PAYLOAD_SIZE = 0x20000;

    explicit UpdateDescriptorQueue(const Device& device_, size_t frame_payload_size_,
                                   bool supports_descriptor_buffer_ = false);
    ~UpdateDescriptorQueue();

    [[nodiscard]] bool UsesDescriptorBuffer() const noexcept {
        return use_descriptor_buffer;
    }

    void TickFrame();
    void Acquire(Scheduler& scheduler, size_t required_entries = 0,
                 bool use_descriptor_buffer_ = false);

    const DescriptorUpdateEntry* UpdateData() const noexcept {
        return upload_start;
    }

    size_t UpdateCount() const noexcept {
        return upload_start ? static_cast<size_t>(payload_cursor - upload_start) : 0;
    }

    void AddSampledImage(VkImageView image_view, VkSampler sampler) {
        const VkDescriptorImageInfo info{
            .sampler = sampler,
            .imageView = image_view,
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        };
        if (t_capture_target != nullptr) {
            t_capture_target->push_back(info);
            return;
        }
        *(payload_cursor++) = info;
    }

    void AddImage(VkImageView image_view) {
        const VkDescriptorImageInfo info{
            .sampler = VK_NULL_HANDLE,
            .imageView = image_view,
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        };
        if (t_capture_target != nullptr) {
            t_capture_target->push_back(info);
            return;
        }
        *(payload_cursor++) = info;
    }

    void AddBuffer(VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size) {
        const VkDescriptorBufferInfo info{
            .buffer = buffer,
            .offset = offset,
            .range = size,
        };
        if (t_capture_target != nullptr) {
            t_capture_target->push_back(info);
            return;
        }
        *(payload_cursor++) = info;
    }

    void AddBuffer(VkBuffer buffer, VkDeviceAddress base_address, VkDeviceSize offset,
                   VkDeviceSize size) {
        if (!use_descriptor_buffer) {
            AddBuffer(buffer, offset, size);
            return;
        }
        const DescriptorAddress info{
            .address = base_address == 0 ? 0 : base_address + offset,
            .range = base_address == 0 ? VK_WHOLE_SIZE : size,
            .format = VK_FORMAT_UNDEFINED,
        };
        if (t_capture_target != nullptr) {
            t_capture_target->push_back(info);
            return;
        }
        *(payload_cursor++) = info;
    }

    void AddTexelBuffer(VkBufferView texel_buffer) {
        if (t_capture_target != nullptr) {
            t_capture_target->push_back(texel_buffer);
            return;
        }
        *(payload_cursor++) = texel_buffer;
    }

    void AddTexelBuffer(VkBufferView texel_buffer, VkDeviceAddress base_address,
                        VkDeviceSize offset, VkDeviceSize size, VkFormat format) {
        if (!use_descriptor_buffer) {
            AddTexelBuffer(texel_buffer);
            return;
        }
        const DescriptorAddress info{
            .address = base_address == 0 ? 0 : base_address + offset,
            .range = base_address == 0 ? VK_WHOLE_SIZE : size,
            .format = format,
        };
        if (t_capture_target != nullptr) {
            t_capture_target->push_back(info);
            return;
        }
        *(payload_cursor++) = info;
    }

    /// Appends a descriptor entry the buffer half resolved earlier (see t_captured_entries).
    void AddEntry(const DescriptorUpdateEntry& entry) {
        *(payload_cursor++) = entry;
    }

    static void BeginCapture() {
        auto& storage = CapturedStorage();
        storage.clear();
        t_capture_target = &storage;
    }

    static void EndCapture() {
        t_capture_target = nullptr;
    }

    static std::vector<DescriptorUpdateEntry>& CapturedEntries() {
        return CapturedStorage();
    }

private:
    const Device& device;
    const size_t frame_payload_size;
    const bool supports_descriptor_buffer;
    bool use_descriptor_buffer{false};
    size_t frame_index{0};
    DescriptorUpdateEntry* payload_cursor = nullptr;
    DescriptorUpdateEntry* payload_start = nullptr;
    const DescriptorUpdateEntry* upload_start = nullptr;
    std::unique_ptr<DescriptorUpdateEntry[]> payload;
};

// TODO: should these be separate classes instead?
using GuestDescriptorQueue = UpdateDescriptorQueue;
using ComputePassDescriptorQueue = UpdateDescriptorQueue;

} // namespace Vulkan
