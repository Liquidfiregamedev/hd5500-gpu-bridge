# Compact-buffer coordinate audit

V8's real laptop logs show 343–344 native buffers with approximately 4.24 GB of owned native descriptor capacity. Immediately before the first failed CreateBuffer, available process commit is 6.4–8.4 MB. Tiny buffers each duplicate an 8 MiB Vulkan/VMA parent allocation.

## Coordinates that must remain distinct

- `Buffer::getMemoryOffset()` and `ObjectMemory::offset` remain the **Vulkan parent-allocation binding offset**. They index the CPU shadow, not the compact D3D resource.
- `Buffer::getNativeOffset()` is explicitly **zero**. Each native buffer stores only its own aligned Vulkan requirements.
- `ObjectMemory::allocateInfo` retains the original parent allocation and memory type. Its `size` remains the bound buffer's aligned requirements.
- `ID3D11Buffer::ByteWidth` becomes that buffer's requirements size, not its parent pool size. The CPU shadow remains parent-sized when host-visible.

## Required corrections implemented in v9

`BufferDeviceMemory` takes native size separately, creates that width, and retains parent allocation metadata. Object upload/download intersects parent-shadow request ranges with the bound child interval; CPU pointers use parent coordinates while mapped resource pointers and native update boxes subtract the binding offset. Dynamic WRITE_DISCARD restores the entire child only. Native readback clones the compact resource descriptor.

`Buffer::copyFrom` keeps parent offsets for shadow `updateData` and uses native-local coordinates for the actual source box and destination. Raw storage UAV FirstElement uses native zero; length remains the logical buffer length rounded to DWORD units.

`CopyBufferToImageCommand` maps `ObjectMemory`, which is a native resource: its source box therefore adds `getNativeOffset()`, not the parent binding offset.

**Important correction to the initial audit:** `CopyImageToBufferCommand::doMapCopy` maps and flushes `DeviceMemory`, which is the parent CPU shadow, not a native resource. Its destination box correctly includes `getMemoryOffset()` and must stay parent-relative. The subsequent staging-buffer native copy is rebased separately by `Buffer::copyFrom`. The image source's parent/subresource offset also stays unchanged.

Mapped `MemoryBarrierCommand` selects the specific buffer ObjectMemory and supplies a buffer-relative offset to the object-specific synchronization overload, rather than treating it as a parent-wide range. Whole-size parent synchronization is normalized before interval arithmetic, preventing unsigned overflow. The child range helper bounds the result against both native width and the remaining parent allocation.

Native vertex/index binding, uniform descriptor static/dynamic ranges, texel-view offsets, indirect argument offsets and direct update/query-copy boxes already use **Vulkan-buffer-relative** coordinates. With native base zero, their existing arithmetic is appropriate; no artificial parent offset is added. CPU update/fill/query lock/flush calls retain their parent offsets. `D3D11ResourceMap.hpp` maps the actual native resource and already clones its real descriptor; it needs no new parent-coordinate rule.

## Remaining prototype boundaries

The pre-existing fill command ignores `dstOffset`; dynamic storage descriptor offsets and some D3D11.0 nonzero uniform ranges have limitations. These are not silently claimed fixed by compact backing. Intentional aliasing of GPU-only Vulkan allocations and complete Vulkan conformance are not established. Image native representation, compressed mip-prefix handling, legacy heap/capability metadata and live-parent CPU shadow sizing are unchanged.

The original read-only review recommended a broad future draw/descriptor/indirect validation matrix. The current candidate acceptance work is narrower and explicitly documented: retain all eight existing graphics regressions, simultaneous live transfer/storage buffers with exact full-parent comparison and slot reuse, and real compute readback from a nonzero-bound raw storage buffer. No claim of exhaustive drawing/API conformance or native Intel v9 gameplay follows from those tests.
