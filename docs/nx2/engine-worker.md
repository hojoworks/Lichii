# Parallel engines: copy/2D/compute worker

Status: design draft, not implemented.

## Why

The Switch and Switch 2 GPUs expose several independent engines behind one PFIFO
front end. The emulator runs them one after another on one thread. This document
maps every place the emulator enters those engines, measures where the CPU time
actually goes, and drafts the thread boundary that would let independent engine
work run beside 3D.

## Hardware model

Both consoles submit through PFIFO: a GPFIFO ring per channel, and every command
word carries a subchannel that routes it to an engine.

| | Switch 1 | Switch 2 |
|---|---|---|
| SoC / GPU | Tegra X1, GM20B | Tegra T239, GA10B class |
| GPU | Maxwell, 2 SM / 256 cores | Ampere, 12 SM / 1536 cores |
| 3D | `MAXWELL_B` | `AMPERE_B` |
| Compute | `MAXWELL_COMPUTE_B` | `AMPERE_COMPUTE_B` |
| Copy | `MAXWELL_DMA_COPY_A` | `AMPERE_DMA_COPY_B` |
| 2D | `FERMI_TWOD_A` | `FERMI_TWOD_A` (unchanged class) |
| Inline to memory | `KEPLER_INLINE_TO_MEMORY_B` | same |

The engines are separate blocks. Copy and 2D run beside 3D; Ampere can run
compute beside 3D. The hardware only serialises at an engine switch, where it
inserts a wait and flushes caches. Concurrency across channels is real; within
one channel's command stream the switch wait serialises the engines.

## Emulator model today

Three threads already exist:

1. **Front** (`gpu_thread.cpp`, `ThreadManager::StartThread`). Takes the CPU's
   command queue, calls `Tegra::Control::Scheduler::Push`, parses GPFIFO and
   command lists, runs macros (`GpuSplit::FrontMacros`).
2. **Back** (`gpu_split.cpp`, `GpuSplit::BackLoop`). Executes engine method
   writes in order: `Maxwell3D` / `KeplerCompute` / `Fermi2D` / `MaxwellDMA`
   `CallMethod`, and everything behind them, including the rasterizer and the
   caches.
3. **VulkanWorker** (`vk_scheduler.cpp`, `Scheduler::WorkerThread`). Pops
   `CommandChunk`s recorded by the back, executes the lambdas into one
   `VkCommandBuffer`, and submits.

Without the split there is one GPU thread and no back; the same work runs inline.

Ownership rules that matter:

- The buffer, texture and query caches are back-owned. The split's front never
  touches them directly (`GpuSplit::Run` defers work and drains).
- The `Scheduler` is owned by whichever thread records. `Scheduler::Record`
  only appends a lambda; `Scheduler::Flush` submits.
- `MasterSemaphore` always submits to `Device::GetGraphicsQueue()`; binary
  signal/wait semaphores are already supported by `Flush`.

## Where each engine is entered

| Engine | Trigger | Engine code | Rasterizer entry | Final Vulkan work |
|---|---|---|---|---|
| Fermi2D | write to `pixels_from_memory.src_y0 + 1` | `Fermi2D::Blit` — `src/video_core/engines/fermi_2d.cpp:64` | `RasterizerVulkan::AccelerateSurfaceCopy` — `vk_rasterizer.cpp:2087` → `TextureCache::BlitImage` — `texture_cache.h:1081` | `TextureCacheRuntime::BlitImage` — `vk_texture_cache.cpp:2207`: `BlitColor` or `scheduler.Record` with `vkCmdBlitImage` / `vkCmdResolveImage` |
| MaxwellDMA | write to `launch_dma` | `MaxwellDMA::Launch` — `maxwell_dma.cpp:65` | `AccelerateDMA::BufferCopy` / `BufferClear` / `ImageToBuffer` / `BufferToImage` — `vk_rasterizer.cpp:2273-2332` | `BufferCache::DMACopy` / `DMAClear`; `TextureCache::DmaBufferImageCopy`, `UploadMemory`, `DownloadImageIntoBuffer` |
| MaxwellDMA (block paths) | same | `CopyBlockLinearToPitch`, `CopyPitchToBlockLinear`, `CopyBlockLinearToBlockLinear` — `maxwell_dma.cpp:187-376` | none | host CPU: `UnswizzleSubrect` / `SwizzleSubrect` over guest memory |
| KeplerCompute | QMD launch | `KeplerCompute::CallMethod` → `DispatchCompute` — `kepler_compute.cpp:567,1084` | `RasterizerVulkan::DispatchCompute` — `vk_rasterizer.cpp:1361` | `pipeline->Configure` + `scheduler.Record` with `vkCmdDispatch` / `DispatchIndirect` |
| Maxwell3D draw | draw methods | `DrawManager::DrawBegin/DrawEnd` → `RasterizerVulkan::Draw` — `draw_manager.cpp:346`, `vk_rasterizer.cpp:651` | — | draw setup + `scheduler.Record` |
| Maxwell3D clear | clear methods | `DrawManager` → `RasterizerVulkan::Clear` — `draw_manager.cpp:96`, `vk_rasterizer.cpp:1136` | — | deferred render-pass clear |
| Inline to memory | `KEPLER_INLINE_TO_MEMORY_B` methods | `KeplerMemory::CallMethod` | `RasterizerVulkan::AccelerateInlineToMemory` — `vk_rasterizer.cpp:2122` | `BufferCache::InlineMemory` or `gpu_memory.WriteBlockUnsafe` |

Subchannel routing: `Puller::CallEngineMethod` — `src/video_core/engines/puller.cpp:455`
reads `bound_engines[subchannel]` and calls that engine.

Texture upload paths (not a guest engine, but the same worker territory):

- `TextureCache::UploadLayers` and `PrepareImage` — `texture_cache.h`
- `TextureCache::QueueAsyncUnswizzle` / `TickAsyncUnswizzle` — `texture_cache.h:2359,2412`
- `TextureCache::QueueAsyncDecode` / `TickAsyncDecode` — `texture_cache.h:2315,2373`
- ASTC decode worker already exists (`texture_decode_worker`).

## Measured profile (OoT, `nx2-588bfa611b`, Kokiri Forest)

`EDEN_NX2_GPU_PROFILE=1 EDEN_DKB_FRAME_TRACE=1`.

Single thread, High accuracy (split disabled by `Settings::IsGPULevelHigh()`):

- 35.5 FPS, `host_cpu_busy_ms_per_s=751`
- `push 340`, `method 202`, `macro 112`, `bind stage buffers 69`, `fence 45`,
  `compute 37`, `update buffers 35`, `flush work 25`, `draw 23`, `read handles 20`,
  `fill image views 14`, `dynamic state 14`, `configure 13`, `pipeline lookup 9`
- 5.26 M 3D register writes/s, 3.60 M of them the same value.

Split, Low/Fast (macros on the front):

- 45.9 FPS
- front: `macro 117`, `push 55`, `split drain 542` (sites: `macro dma read 326`,
  `acquire 212`, `list writers 167`)
- back: `method 254`, `draw 88`
- 2068 draws per frame, 64 K draws/s; 1423 submits/s, 5900 render passes/s.

Per-thread CPU from `DKB cpu` (38-frame window, ~1022 ms):

```
gpu_command_cpu_ms = 843  (84%)   front
gpu_back_cpu_ms    = 921  (92%)   back
vulkan_worker_cpu_ms = 250 (25%)  command buffer recording + submit
```

Two conclusions drive the design:

1. The **back is the wall**; the VulkanWorker has three quarters of its time
   free. Moving Vulkan recording to another thread cannot help.
2. The copy/2D engines are a small share of OoT's frame. The large items are
   per-method and per-draw CPU work on the back: ~254 ms/s of method dispatch
   and ~190 ms/s of draw setup (pipeline lookup, handles, buffers, binding).

So a worker is worth building only if it takes **whole engine work** (cache
bookkeeping included) off the back, not just the final `vkCmd*` calls. It buys
single-digit percent on OoT and considerably more on titles that run compute and
copy queues beside 3D (Mario Kart World drives ~200 ms/s of macros and large
compute/copy traffic).

## Worker design

### Shape

One `EngineWorker` thread, owning a transfer-only scheduler and a job queue.
The back stays the sole owner of 3D state and of the draw stream.

```
front ── ring ──► back ──┬─ 3D: draws/clears ──► scheduler ──► VulkanWorker ──► graphics queue
                         └─ copy/compute job ──► EngineWorker ──► transfer scheduler ──► queue
```

Both schedulers submit to the same `VkQueue`. Ordering is by semaphore, not by
submission order, so the back never blocks on the worker unless the guest's
dependencies require it.

### Job types

```cpp
struct BlitJob {                     // Fermi2D::Blit
    Tegra::Engines::Fermi2D::Surface src, dst;
    Tegra::Engines::Fermi2D::Config config;
    u64 dependency_tick;             // shared timeline value the copy must follow
};

struct DmaJob {                      // MaxwellDMA::Launch, non-block path
    MaxwellDMA::Regs regs;           // snapshot, worker owns it afterwards
    u64 dependency_tick;
};

struct ComputeJob {                  // KeplerCompute dispatch
    KeplerCompute::Regs regs;        // QMD + launch description snapshot
    u64 dependency_tick;
};

struct UploadJob {                   // inline-to-memory / deferred texture upload
    GPUVAddr address;
    std::vector<u8> data;
};
```

The first three carry copies of the engine registers the back already has. No
job references back-owned state after enqueue.

### Ordering

Use the existing timeline. Both schedulers share one `MasterSemaphore`:

- `Scheduler` gains a constructor that borrows a `MasterSemaphore&`.
- `Flush` / `SubmitExecution` / `MasterSemaphore::SubmitQueueTimeline` gain a
  timeline **wait value** (today the wait value is hard-coded to 0, which only
  works for binary semaphores).
- The back records its current tick `T` when it enqueues a job.
- The worker submits with `wait_value = T`; it returns `C`, the tick its own
  copy signals.
- The back records `wait_value = C` before the first later command that can
  read the copy's destination: at minimum at the next `Flush()`, at best at the
  first draw whose descriptors cover the destination range (the texture/buffer
  caches know their ranges).

For v1, do the conservative thing: the back waits at the next engine switch or
the next `Flush`. That already overlaps the copy with the draws between the
enqueue point and the next submission, and it is trivially correct. Tighten to
range-based waits once the prototype is measured.

### Cache access

The worker runs the existing copy entry points unchanged:

- `RasterizerVulkan::AccelerateSurfaceCopy` takes `texture_cache.mutex`.
- `AccelerateDMA::*` takes `buffer_cache.mutex` and/or `texture_cache.mutex`.

So the worker and the back serialise on those mutexes, but only for the copy's
own cache work, not for a whole draw. Lock order must match the back's:
`buffer_cache.mutex` then `texture_cache.mutex` (see
`AccelerateDMA::DmaBufferImageCopy`, `vk_rasterizer.cpp:2293`).

The worker must never touch:

- `GpuSplit` ring state or `DmaPusher` state;
- the back's `Scheduler`;
- `Maxwell3D` / `KeplerCompute` register state (jobs carry snapshots);
- the query cache's commit thread (`Finish`, `TickFrame`).

### Transfer scheduler

Copy work needs a command buffer, a timeline and a submit, but no render pass,
no pipeline state and no query cache. Two options:

1. Add `Scheduler::Mode::Transfer` that skips `EndPendingOperations`,
   `InvalidateState`, `state_tracker` and `query_cache` calls. ~30 lines of
   conditionals, reuses command pool and submission code.
2. Write a small `TransferScheduler` beside `Scheduler` with a command pool, a
   borrowed `MasterSemaphore` and `SubmitExecution`'s submission block.

Prefer (1) for the prototype: less duplicated submit code to keep in sync.

### Flag and measurement

- `EDEN_NX2_ENGINE_WORKER=1` turns it on; default off until proven.
- Add profile zones for the copy engines before and after: `Fermi2D blit`,
  `DMA launch`, `AccelerateSurfaceCopy`, `AccelerateDMA`, `inline upload`.
  Today none of these are instrumented, so the copy share is invisible.
- A/B with `bench.py` in Low+split and High; screenshot diff for the blits and
  uploads (visually obvious if ordering is wrong).
- Counter for jobs per second by type, and for cross-lane waits.

## Probe results (2026-11-06, `nx2-588bfa611b`, OoT Low+split)

Two instruments were added behind environment flags and are kept in the tree:

- `EDEN_NX2_CMD_PROBE=1` hashes every submitted command list at
  `nvhost_gpu::SubmitGPFIFOImpl` and classifies repeats per frame
  (`src/video_core/nx2_cmd_probe.h`).
- `EDEN_NX2_FAST_PROFILE=1` counts and times the fast Maxwell3D path without
  disabling it (`src/video_core/nx2_profile.h`). `EDEN_NX2_GPU_PROFILE` cannot
  do this: `maxwell_3d.cpp:61` turns the fast path off when it is set, which is
  why every earlier zone profile describes a different code path.

Command stream:

| Measure | Value |
|---|---|
| command-list entries | ~520,000 per 2 s (≈4,400 per frame), ~67 MiB |
| content new this session | ~22 % |
| content seen in a previous frame | ~37 % |
| content repeated inside the same frame | ~41 % |
| same address, same content | ~59 % |
| same address, different content | ~39 % |
| whole submissions repeated across frames | **0 %** |

So a whole-frame replay cache is dead: the guest rewrites its command buffers
every frame. A per-list cache could cover roughly a third of entries, which is
useful but not the big win.

Fast path (draw time included in the method timer):

| Measure | Value |
|---|---|
| back-thread CPU | ~875 ms/s (90 %) at 58 fps without instrumentation |
| method stream | ~613 ms/s, ~4.6 M methods/s |
| **draws** | **~403 ms/s, ~81,500/s, ~4.9 µs per draw** |
| non-draw method writes | ~211 ms/s |
| direct (macro) / sunk / executed | 80 % / 10 % / 3 % of methods |

The wall is per-draw setup: about 45 % of the back thread and 65 % of the method
stream. The register-write stream is a secondary target and the copy engine is
noise.

### Draw-state probe (corrected)

`EDEN_NX2_DRAW_PROBE=1` hashes pipeline, draw parameters, render target,
buffers, viewport/scissor and texture handles — and the per-draw constant
buffer bindings, which turned out to be decisive:

| Measure | Value |
|---|---|
| new state | 59-81 % (scene dependent) |
| repeat from a previous frame | **18-41 %** |
| repeat inside the same frame | 0.4 % |
| consecutive identical state | **0.1 %** |
| consecutive same pipeline | **72.4 %** |

Without the constant buffer bindings the probe reported 27 % consecutive
identical draws; with them it is 0.1 %. So the apparent batching jackpot was an
artifact: every draw binds different per-object constant buffers even when the
pipeline is the same. **Multi-draw batching of identical draws and a same-frame
draw cache are dead.**

What the numbers actually say:

- The pipeline is stable across 72 % of consecutive draws, but the per-object
  constant buffers, vertex ranges and textures change every draw.
- ~40 % of full draw states (bindings included) return in a later frame, but the
  data behind those bindings is rewritten.
- The per-draw cost is therefore genuine per-object work — streaming and binding
  constants — not redundant state.

That leaves one big lever: **execute the per-draw work on more than one thread**.


Slow-path sub-zones, which match the per-draw cost, ranked:

| Sub-zone | Share of draw setup |
|---|---|
| `bind stage buffers` | 32 % |
| `update buffers` | 17 % |
| `read handles` | 10 % |
| `fill image views` | 7 % |
| `dynamic state` | 7 % |
| `configure` | 6 % |
| `pipeline lookup` | 4 % |
| draw record + rest | 17 % |

## Parallel draw resolver v1 (implemented, measured, not a win)

`EDEN_NX2_PARALLEL_DRAW=1` splits `GraphicsPipeline::ConfigureImpl` by cache
domain: a persistent `DrawWorker` resolves the buffer half (SSBO resolution,
buffer updates, geometry and stage buffer binds) while the back resolves the
texture half. Buffer descriptor entries are captured into a shared buffer and
scheduler writes (uploads, geometry binds) are deferred; the back appends and
replays both after the join. It renders correctly.

| Variant | FPS (Low+split, no profiler) |
|---|---|
| serial baseline | 58.4 (later run of the same scene: 52.5) |
| parallel, mutex/condvar handoff | 13.8 |
| parallel, spin handoff | 46.9 |
| parallel, spin + fast profile | 44.2 |
| parallel, spin, stable order | 51.3 |

Starting the worker **before** the texture half (to force overlap) crashed with an
access violation (`0x130` read) inside `nvoglv64.dll` while the intro was
decoding. Two root causes were found and fixed, and it still crashed:

- VMA is created with `VMA_ALLOCATOR_CREATE_EXTERNALLY_SYNCHRONIZED_BIT`, so the
  allocator is not thread-safe; a global `VmaMutex()` now covers allocation,
  destruction, mapping and sparse memory (`vma_lock.h`).
- `StagingBufferPool` (stream cursor, per-level caches) had no lock; it now has
  one.

The crash survived both, so more of the renderer's Vulkan state is shared than
the caches show (command pools, descriptor ring, video decoder, memory manager).
A second renderer thread needs a full thread-safety audit, not a split.

**Decision: the concurrent draw resolver is abandoned for now.** The stable
order (worker after the texture half) remains compiled behind
`EDEN_NX2_PARALLEL_DRAW` as an experiment; it is at parity (51.3 vs 52.5 fps)
and must not be enabled. The default serial path is the supported one.

The VMA lock and the staging pool lock are kept: they are correct on their own
and make future concurrent work possible.


Fast-path timing per draw (stable order):

- back waits for the worker: **2.9 µs** (215 ms/s of the back)
- worker's CPU: **2.5 µs/job** (181 ms/s)
- back's own half: **~3 µs**
- serial draw: 5.8-6.0 µs

The worker is not slow; the problem is ordering. In the stable order the back
completes its whole texture half before starting the worker, so there is
nothing to overlap and the wait is the worker's entire job. Starting it earlier
overlaps but races the driver state.


Also checked: `VK_EXT_external_memory_host` is **not supported** by the device
(`vulkan_device.cpp:1190`), so mapping guest memory directly into Vulkan
buffers (zero-copy uniforms) is not available here.

### What this means

The draw cost is genuine per-object data-plane work. The remaining parallel
option is a **one-batch pipeline** that overlaps a whole batch instead of a
single draw:

1. Back: resolve the texture half for draws 0..K-1 of batch B (same render
   target, same pipeline, no storage writes), buffer the pending draws.
2. Back: submit one worker job for all K buffer halves of batch B.
3. Back: while that job runs, record batch B-1 from its stored captures,
   descriptor entries and draw functions.
4. Next batch: join B, start C, record B.

This keeps the cache order (texture before buffer per batch) and never lets the
worker and back touch the same Vulkan state concurrently, so it avoids the
crash above. Expected: back does texture (K x 1.5 us) plus record (K x 2 us)
while the worker does buffer (K x 2.5 us); the draw stage drops from ~6 us to
~3.5 us per draw, roughly +15-20 % fps.

It requires a genuine deferred-draw queue in the rasterizer: pending draws,
stored `draw_func`, per-draw descriptor `Acquire` at record time, and flushes at
every state boundary (clear, dispatch, target change, fence, frame end).

### The GPU-bound question

The host GPU is at ~20 % because the CPU is translating ~1,500 draws/frame.
The batch above is still CPU work, just overlapped. Becoming GPU-bound needs a
different design: GPU-driven draws (`VK_EXT_multi_draw` plus a per-draw
parameter buffer, and `gl_DrawID`-indexed constants/descriptors so a whole run
is one Vulkan command). That is a shader-translation and descriptor-model
project, not a threading one. The batch pipeline is the right next step and
makes that project easier, because it introduces the deferred draw packet the
GPU-driven path needs.





## Risks

- **Deadlock on cache mutexes.** The split-at-High experiment already deadlocked
  this way: the front took a cache mutex and waited on the back, which needed
  the same mutex. The worker must never hold a cache mutex while waiting on a
  scheduler tick or on the back.
- **Stale reads across lanes.** A 3D draw sampling a texture the worker is still
  copying must wait. Conservative ordering in v1; range-based later.
- **Queue submission serialisation.** Both schedulers share the graphics queue;
  `vkQueueSubmit` needs external synchronisation. Share one submit mutex, or
  give each scheduler its own mutex and rely on semaphores for ordering (the
  Vulkan spec allows concurrent submits from different threads with different
  mutexes).
- **Fence/query release.** `ReleaseFences`, `SignalFence` and query results are
  back-owned. The worker must not signal guest fences; the back does that after
  its wait.
- **Frame end.** `TickFrame` frees resources and ticks caches; the worker must
  be idle (or drained) before it runs, or resources can be freed under it.
