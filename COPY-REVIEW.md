# BC copy review — HD 5500 v5

**Use `pSrcBox = nullptr` for a per-layer BC image copy only when it is an exact whole-native-subresource copy:** the source and destination offsets are `(0,0,0)`, the requested extent equals both the source and destination **native D3D mip** extents, and the extent depth matches (one for these 2D images). Otherwise retain the explicit logical `D3D11_BOX`. This is legal D3D11 and is the most narrowly correct change for the reported 6×6, 3×3, and 1×1 tail mips.

`CopySubresourceRegion` explicitly defines `pSrcBox == NULL` as copying the *entire source subresource*. With source and destination origins at zero and equal native extents, that implicit source extent fits the matching destination subresource. It therefore meets the normal source-box/destination-bounds contract without inventing any coordinates. The same API says texture coordinates are texels, requires an explicit box to fit in the source, and warns that invalid source/destination bounds have undefined behavior.[1]

This is preferable to rounding `right`/`bottom` up to 4 for a logical 6×6, 3×3, or 1×1 mip. Direct3D documents that BC mips have a declared/virtual size and a physically padded allocation, including a full 4×4 block for mips smaller than 4×4, but that **no API exposes the physical padded size**. Its guidance for region operations is to respect aligned compressed blocks, not to claim padded texels outside the declared resource.[2] An explicit logical box ending at 6, 3, or 1 leaves the driver to handle the edge block; an explicit box ending at 8 or 4 can exceed the exposed source size and risks the undefined behavior above. **Do not add blanket 4×4 copy padding in the logical domain.**

The shared source as inspected already contains this intended shape: `D3D11CopyImageCommand.cpp` gets each layer's native subresource, obtains `Texture2D::GetDesc()` dimensions indexed by that native mip, and selects `wholeBc ? nullptr : &m_srcBox`. `D3D11BcCopy.hpp::wholeSubresource` checks the BC1–BC7 range, both origins, source and destination extents, and depth. That is the correct predicate structure. Keep the decision **per evaluated layer**, as it is now, because native subresource indices include the BC mip prefix. Do not compare a logical mip index to a native `D3D11_TEXTURE2D_DESC` mip index directly.

ANGLE provides useful precedent. Its D3D11 texture-storage path computes `fullCopy` by testing zero origin and equality to the actual level size, then passes `NULL` for a full copy; it uses a box only for a partial copy. For its explicit compressed copy box, ANGLE separately rounds the copied width and height to the DXGI block dimensions.[3] That is corroboration for the whole-copy null-box representation, **not** a reason to round arbitrary Ashes logical-copy extents. ANGLE has a different image model and its rounding is only safe within its own bounds/invariants.

## Why this matches the v5 evidence

The supplied BC7 probe created and bound all 48 requested images and created all 100 full/tail SRVs. Its exact block-byte check passed for mip 0 (24×24) and mip 1 (12×12) for both UNORM and sRGB, then failed for mip 2 (6×6, 2×2 blocks), mip 3 (3×3, one block), and mip 4 (1×1, one block). The run reports six mismatches, two upload-case failures, and zero Vulkan debug errors. This concentrates the fault at the first non-4-divisible tail mip rather than at format capability, memory binding, or view creation. The stated v5 Wine tests also pass, so they do not contradict an adapter/driver-specific native path issue.

The similarity to an older Intel community reproduction is significant but not proof of the exact HD 5500 issue. That Windows/D3D11 report on Intel HD 530 found a BC1 `CopyResource` test correct at mips 0–1 and corrupt at mips 2–3, while NVIDIA and AMD did not reproduce it.[4] It makes a silent Intel compressed-tail-mip driver defect plausible. It also means the null-box selector is **necessary/appropriate for correct API expression, but not sufficient evidence of a complete HD 5500 workaround**: `CopyResource` is already an all-subresource path and that historical report still failed. Re-run the exact two-format probe after the selector change and treat all five mips matching as the acceptance condition.

## Remaining risk and scope boundary

The selector deliberately does **not** fix partial BC copies, copies with non-zero origins, dimension-changing copies, 3D images, format conversion, or a native source/destination extent mismatch. Those must continue to use an explicit box and retain their existing validation requirements. This is correct: the null form means whole source subresource, so it cannot represent a partial or offset operation.

The remaining primary risk is a driver fault even on a whole-subresource copy, as the Intel HD 530 report suggests. A secondary risk is that a call site expects an image's logical dimensions but the native texture has a hidden prefix; using the actual native subresource and `GetDesc()`-derived native mip extent avoids this. Keep the existing trace gated by `RW_HD5500_TEXTURE_TRACE=1` during the rerun so the failing test can prove which mips selected `whole-null-box`; it should show the actual native subresources and logical origins/extents.

## Native debug-layer collector

**Do not make a native D3D11 debug-layer collector a prerequisite for this selector change or use it as the upload-vs-readback discriminator.** It is useful as optional, opt-in validation: it can capture a runtime complaint about an out-of-bounds/otherwise-invalid `CopySubresourceRegion` call and attach the native source/destination subresources, offsets, and whether the box was null. That would be valuable if the post-change rerun emits messages.

It cannot, however, localize this failure class. The observed calls can be legal and still silently return altered compressed bytes; `CopySubresourceRegion` has no HRESULT and the D3D debug layer validates calls rather than comparing BC contents. The v5 zero-error result is therefore unsurprising and does not clear the driver path.

If the null-box rerun still mismatches, add a small directed native-D3D integrity probe rather than broad collector work: for every tail mip, compare a readback after upload to source bytes, then compare a readback after the image-to-image copy to the same bytes, with the phase and `nullptr`/box mode logged. This is the test that can narrow upload, image copy, or readback; capture debug-layer messages alongside it only as supplementary validation. No production fallback should be selected until that test identifies a working transfer path.

## References

[1]: https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-copysubresourceregion "ID3D11DeviceContext::CopySubresourceRegion — Microsoft Learn"

[2]: https://learn.microsoft.com/en-us/windows/win32/direct3d10/d3d10-graphics-programming-guide-resources-block-compression "Block Compression (Direct3D 10) — Microsoft Learn"

[3]: https://chromium.googlesource.com/angle/angle/+/c67e6e9fade44ef8938724e82db11db725e9c8e5/src/libANGLE/renderer/d3d/d3d11/TextureStorage11.cpp "ANGLE D3D11 TextureStorage copy implementation"

[4]: https://community.intel.com/t5/Graphics/D3D11-CopyResource-incorrectly-copying-BC1-textures/m-p/1119562 "Intel community report: D3D11 CopyResource incorrectly copying BC1 textures"
