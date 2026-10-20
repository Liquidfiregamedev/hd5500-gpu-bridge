# V11 changes

## Combined correction

- **Compact destination scratch:** a supported compute blit into a large non-storage atlas uses a destination bounding-box scratch texture rather than automatically cloning the entire atlas. Original atlas dimensions and contents remain unchanged.
- **Checked coordinate mapping:** scratch-local mip/layer and rectangle coordinates are translated explicitly back to the original atlas. Mixed destination mips and cube-compatible shapes retain an explicit full-size fallback rather than pretending to be compact.
- **Safe image backing release:** image destruction releases its owned native backing while retaining stable parent-pool metadata. Weak parent-lifetime tokens guard unused parent-free-first cleanup. Borrowed DXGI/swapchain textures are not released by this path.
- **Stronger tests and catches:** retain all existing byte/pixel, BC7, buffer, clone/reset and failure-path checks; add full 4096-atlas batch readback, unchanged guards, mip/layer neighbors, retained-command memory accounting and pooled-image reuse.

## Four additional efficiency improvements

1. **Reuse immutable blit samplers.** Nearest and linear sampler owners are cached per blit pipeline with shared ownership so cloned commands cannot outlive their sampler.
2. **Reduce normal diagnostic overhead.** Normal launch omits full command/loader/resource-name tracing and optional native debug-layer validation, while retaining actual failure reasons and bounded allocation evidence. Verbose and serialized diagnostics remain explicit alternatives.
3. **Centralize layout and diagnostic setup.** A small scratch-layout planner handles bounds and coordinate transforms; one launcher helper switches process-local diagnostic settings consistently. Focused unit tests exercise both helpers.
4. **Measure repeated batches.** The new probe reports record, submit/wait and verification timings for the first and reused batch. Timing is diagnostic only and never replaces correctness pass criteria.

## Deliberate limits

This is a targeted cleanup, not a new Vulkan implementation or a rewrite of RimWorld's engine. No TDR, pagefile, registry, save, mod, driver or permanent environment changes are introduced. Compact-scratch counters measure RGBA8 destination texel payload, not total driver residency, process memory, source temporaries or full-fallback allocations. Passing a Windows-DLL integration test under Wine is not proof of native Intel game compatibility or a game FPS increase.
