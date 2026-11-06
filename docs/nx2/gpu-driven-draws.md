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

## Implementation map (next commits)

### Host: DrawParams set 1

- `src/video_core/renderer_vulkan/pipeline_helper.h`
  - add `vk::DescriptorSetLayout draw_params_layout` (one `VK_DESCRIPTOR_TYPE_STORAGE_BUFFER`,
    binding 0, `VK_SHADER_STAGE_ALL_GRAPHICS`);
  - `CreatePipelineLayout(VkDescriptorSetLayout set0, VkDescriptorSetLayout set1)` with
    `setLayoutCount = 2` (or 1 when set1 is null).
- `src/video_core/renderer_vulkan/vk_graphics_pipeline.cpp`
  - create the set-1 layout in the constructor next to `descriptor_set_layout`;
  - pass it to `CreatePipelineLayout`;
  - keep a `vk::DescriptorSet draw_params_set` from a small pool (or push it) and update it
    with the DrawParams buffer handle once per batch;
  - bind set 1 in the draw/batch record lambda.
- `src/video_core/renderer_vulkan/vk_buffer_cache.*` (or a small owner in the rasterizer)
  - a host-visible, coherent buffer of `u32 cb_base[MAX_CBUFS]` per draw, grown as needed;
  - `BindDrawParams(buffer, range)` and `WriteDrawParams(index, bases)`.
- `src/video_core/renderer_vulkan/vk_rasterizer.*`
  - `PendingBatch { pipeline, framebuffer, key, vector<PendingDraw> }`;
  - append when the key matches, flush at pipeline/target/viewport/scissor/texture/storage
    changes, clears, dispatches, fences and frame end;
  - on flush, fill the DrawParams buffer, bind set 1, and issue
    `DrawMultiIndexedEXT` / `DrawMultiEXT`.

### Shader: DrawID and the constant base

- `src/shader_recompiler/backend/spirv/spirv_emit_context.h/.cpp`
  - declare a set-1 storage buffer `DrawParams { u32 cb_base[MAX_CBUFS]; }` (`std430`);
  - vertex stage: `OpVariable` with `BuiltIn DrawIndex` (`SPV_KHR_shader_draw_parameters`);
  - reserve a flat `u32` varying (vertex out, fragment in) carrying the draw index; add it to
    the interface agreement in `pipeline_helper`/`StageInfo`;
  - expose the resolved draw-index `Id` on `EmitContext`.
- `src/shader_recompiler/backend/spirv/emit_spirv_context_get_set.cpp`
  - `GetCbuf`: load `cb_base` for the current draw and add it to `buffer_offset` before the
    access chain (line 136);
  - the indirect CB function (`load_const_func_*`) must add the same base;
  - fragment-stage reads use the flat varying; vertex-stage reads use `DrawIndex`.
- `Profile`/`RuntimeInfo`: enable `SPV_KHR_shader_draw_parameters` and the storage-buffer
  binding for both stages.

### Validation

1. Plumbing with a dummy DrawParams buffer and an unchanged shader: rendering byte-identical.
2. DrawID injection with batches disabled: rendering byte-identical against serial.
3. Batches on, screenshot diff, then `bench.py` Low+split and High.

The first two checkpoints are cheap and catch layout/interface mistakes before any timing
claim. Do not skip them.

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
