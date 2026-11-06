# GPU-driven draws

Status: design, not implemented.

## Why

The host GPU sits at ~20 % while the back thread is ~90 % busy translating
~1,500 draws per frame. The cost is per-draw CPU work (constant buffer stream
and bind, descriptor encode, pipeline/dynamic state), not GPU work. Making the
GPU the bottleneck means issuing far fewer commands per draw and moving the
per-draw variation into GPU-readable buffers.

## Measured batchability (OoT, Low+split, `EDEN_NX2_DRAW_PROBE=1`)

The batch key is what must be identical for one `vkCmdDrawMultiIndexedEXT` run
without bindless textures: pipeline, render target, viewport, scissor, texture
handles, sampler mode. Geometry and constant-buffer bindings may differ.

| Measure | Value |
|---|---|
| draws in runs >= 2 | 76.7 % |
| draws in runs >= 4 | 57.4 % |
| draws in runs >= 8 | **42.7 %** |
| average run | **5.4 draws** |
| runs per second | ~11,500 |

Without textures in the key the runs are longer (83.4 / 69.0 / 54.1 %, average
6.9). Textures cost some batchability; DrawID-indexed descriptors would recover
it later.

Consecutive identical full state is 0.1 %: batching *without* moving constants
to the GPU buys nothing. That is the whole point of the design.

## Design

### Batch accumulation

`RasterizerVulkan` keeps a pending batch:

```cpp
struct PendingDraw {
    DrawParams params;         // vertex/index ranges, instances
    u32 constant_offset;       // where this draw's constants live in the ring
};
struct PendingBatch {
    GraphicsPipeline* pipeline;
    VkFramebuffer render_target;
    u64 key;                   // pipeline + RT + viewport/scissor + textures
    std::vector<PendingDraw> draws;
};
```

A draw is appended when its key matches the open batch and the batch has room;
otherwise the batch is flushed and a new one starts. Flush also happens at every
state boundary that already ends a render pass: clear, dispatch, target change,
fence, frame end, storage-buffer write, image write.

### Per-draw constants without per-draw binds

Today each draw streams its constant buffers and binds a descriptor pointing at
the stream slice. For a batch, bind **one** constant ring for the whole batch
and put the per-draw slice offset in a **draw-parameters buffer** indexed by
`gl_DrawID`:

```
struct DrawParams {
    u32 cb_base[MAX_CBUFS];   // byte offset of this draw's slice in the ring
    u32 pad;
};
// layout.std430, set 3 binding 0, or a device address passed in push constants
```

The shader adds `cb_base[i]` to every constant buffer access. The recompiler
already emits `OpAccessChain` on the UBO; the change is to add a loaded value
(`draw_params[gl_DrawID].cb_base[i]`) to the index. `SPV_KHR_shader_draw_parameters`
provides `gl_DrawID`; pass it from the vertex shader to the fragment shader as a
flat varying where the fragment stage reads constants.

For a first milestone, restrict to shaders whose constant buffers are accessed
through the standard descriptor path (no indirect/calculated CB addresses, no
texture buffers, no storage buffers), which covers the bulk of the draws.

### Issuing the batch

Per draw in the batch, append a `VkMultiDrawIndexedInfoEXT` (or
`VkMultiDrawInfoEXT` for non-indexed): `firstIndex`, `indexCount`,
`vertexOffset`. Then one `vkCmdDrawMultiIndexedEXT(count, info, stride,
drawCount)`. Device support: `VK_EXT_multi_draw` (check in `vulkan_device`).

### Descriptors and textures

The batch binds the pipeline, the constant ring and the draw-params buffer once.
Textures are part of the key, so a batch's descriptor set is constant. Later,
`VK_EXT_descriptor_indexing` (or DrawID-indexed descriptor offsets) lets batches
span different texture sets and recovers the ~10 % of runs lost to texture
changes.

## Milestones

0. **Plumbing.** Enable `VK_EXT_multi_draw`, add the wrapper call, add the
   batch/`PendingDraw` structures and a flush skeleton. Batch only draws with a
   fully identical state (rare) to validate correctness and rendering.
1. **DrawID constants.** Draw-params buffer, constant ring, shader recompiler
   injection for a restricted shader subset. Batch runs >= 2 and A/B measure.
2. **Runs >= 8.** Tune the key and flush policy; measure OoT Low+split and High.
3. **Textures via descriptor indexing.** Recover the runs lost to texture
   changes; extend to storage buffers and texture buffers.

## Risks

- **Recompiler complexity.** Every constant access must add the DrawID base;
  indirect/calculated CB addressing and texture buffers are out of scope for the
  first milestone.
- **Layout and alignment.** The ring slice must satisfy the UBO alignment
  (`minUniformBufferOffsetAlignment`), or use a fixed stride with padding.
- **Correctness.** A batch must not cross a state boundary that changes the
  pipeline, target, viewport, scissor, textures or storage bindings. A missed
  boundary shows as wrong rendering, not a crash, so A/B screenshots and the
  frame dump are required for each milestone.
- **Guest writes between draws.** The constant ring copies each draw's data at
  preparation time; no guest data may be read at GPU execution time.
- **Extension support.** `VK_EXT_multi_draw` is present on the target NVIDIA
  driver; fall back to per-draw commands when absent.
