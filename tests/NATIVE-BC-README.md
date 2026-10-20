# Supplemental native BC copy diagnostic

`NativeBcCopyProbe.exe` uses Direct3D 11 directly, not Vulkan. Its default policy explicitly selects a hardware Intel DXGI adapter (vendor 8086), rejects software/Microsoft Basic adapters, and never tries WARP. The game launcher runs it automatically only after a failed BC7 mip preflight. Its result cannot bypass that failure.

For BC7 UNORM and sRGB, a 24x24 five-mip GPU resource is created with known valid mode-6 BC data supplied as resource initial data. This bypasses staging-to-GPU upload for the readback control. Each mip is copied into a legal CPU-readable/writable staging resource using a whole-subresource null box; native base/mip prefix preserves exact 24, 12, 6, 3 and 1 pixel mip sizes. Exact block bytes are compared after real `Map` calls.

Only if that format's initial-data readback control passes, a separate initialized staging chain is copied to a GPU-default chain and read back. Failure of the first control makes upload localization inconclusive. This controls copy direction; it is not a GPU-decoded-pixel assertion and resource initialization/driver internals can still matter.

Normal invocation:

```bat
tests\NativeBcCopyProbe.exe
```

For a separate, explicitly requested old-path comparison, `--compare-explicit` runs boxed copies **after** all required whole-copy controls. Their comparative results and post-comparison device status are logged, but do not replace the required controls. The automatic launcher does not request these potentially faulty legacy copies.

The sandbox-only `--allow-non-intel-test` permits a different non-software DXGI vendor. It is never used by the Windows launcher. There is no automatic fallback selection.

## Observed sandbox results

The Windows x64 MSVC-ABI executable was built with Clang/MSVC SDK, static C++ runtime and linked D3D11/DXGI, then run under software-backed Wine/Xvfb:

- Required whole-copy mode: 10 initial-GPU readback cases and 10 staging-upload/readback cases exact; exit 0, device status S_OK.
- Optional explicit comparison: 10 additional cases exact in the sandbox; device status S_OK. This does not reproduce or disprove the Intel-specific mismatch.
- No vendor override: the sandbox's synthetic non-Intel adapter is rejected, exit 6. No WARP attempt.

Raw logs are in `final-v6/native-bc-*.log`. Source is `src/native-bc-copy-probe.cpp`. These results are not native Intel hardware validation. A native developer-prompt rebuild command is included, but not executed under Windows here.
