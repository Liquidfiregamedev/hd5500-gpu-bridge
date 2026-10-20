# V11 compact scratch design

The source introduces a small `blitscratch::Layout` value type taking image create-info and the original blit regions. It validates destination rectangles/mips/layers, computes the bounding rectangle for a common mip, and records the minimum referenced layer plus the required interval. Original atlas metadata is not modified.

For supported non-storage destinations, the temporary has one scratch-local mip, bounding-box dimensions and local layers. `makeLocalDestination` adjusts only the temporary destination; `makeCopyBack` restores the original mip/layer/origin and exact requested extent. Disjoint regions are individually dispatched/copied, so bounding-box holes are not copied back into untouched atlas pixels. Existing storage-capable destinations keep their direct path.

A request naming mixed destination mips or a cube-compatible destination uses an explicitly logged full-size fallback. Invalid/unrepresentable shapes fail rather than being silently truncated. Earlier RGBA8/BC7 source and allocation/error restrictions remain. Source sampling temporaries are not universally compacted by this change.

Temporary ownership remains shared across flattened/secondary command clones. The final owner releases dependent child commands/views/descriptors before temporary images and memory. Compact accounting records exactly one allocation/release per shared owner and is protected by a mutex. The first 256 allocation/release records and bounded fallback/failure information are logged; counters describe compact RGBA8 destination texel payload, not full driver residency or all allocations. A large bounding box across distant regions can still be atlas-sized.

Immutable nearest/linear sampler owners are cached per blit pipeline under a mutex and held by dependent commands. This avoids needless recreation without sharing writable scratch between potentially concurrent commands. Existing sampler register/pixel-center behavior remains tested.

The direct planner fixture passed 16 checks. Actual GPU-path integration compared every pixel in two 4096-atlas batches, selected mip/layer neighbors, guard regions and secondary execution. The strict launcher also requires balanced scratch IDs/zero live payload. Full results and the native Intel/game boundary are in `TEST-RESULTS.md`.
