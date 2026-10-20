# Suballocated Compute Storage-Buffer Fixture

`Vulkan10GpuIcdProbeSuballocated.exe` is a **standalone Windows x64 raw-Vulkan fixture**. It is a focused source variant of `/home/ubuntu/rimworld-gpu-d3d11-v8/package/src/vulkan10_gpu_icd_probe.c`; no renderer, core, runtime DLL, or other package file was edited to create it.

## Test target

This fixture isolates the coordinate split required by a compact native-backing implementation:

| Coordinate / operation | Exact fixture behavior |
|---|---|
| Logical Vulkan storage buffer | Unchanged: one 4-byte `VK_BUFFER_USAGE_STORAGE_BUFFER_BIT` buffer, descriptor offset `0`, descriptor range `4`, same embedded SPIR-V 1.0 shader, same device/pipeline setup, one `vkCmdDispatch(1,1,1)`, same barriers, and exact expected readback `0x00005500`. |
| Parent `VkDeviceMemory` | Allocates `VkMemoryRequirements::size + 2 MiB`. |
| Vulkan buffer bind | Binds the logical buffer at **exactly `1 MiB`** into that parent: `vkBindBufferMemory(..., 1048576)`. It rejects a device whose buffer alignment cannot admit that exact bind offset. |
| CPU initialization and readback | Maps the **parent** allocation and accesses `((uint8_t *)mapped + 1 MiB)` for both the initial zero write and the final `0x00005500` readback. Parent byte zero is neither written nor read as the logical storage value. |
| Noncoherent synchronization | Flush/invalidate ranges name the parent `VkDeviceMemory`; their start is rounded down from the 1 MiB parent offset to `nonCoherentAtomSize` and use `VK_WHOLE_SIZE` to the end of that parent allocation. Thus the actual storage word is covered without treating a buffer-relative `0` as a parent-memory offset. |

The intended renderer-side observation is that a raw UAV for this buffer starts at **native byte 0**, while its CPU shadow remains at **parent offset 1 MiB**. This executable does not contain a renderer change and makes no claim that an engine was modified. It is only a raw-Vulkan reproduction target for that distinction.

The default adapter policy remains **non-software Intel vendor ID `0x8086` only**. `--allow-non-intel-test` is still an explicit sandbox-only override; it does not allow CPU/WARP/software adapters and cannot establish Intel/HD 5500 validation. No GPU handles, device features, output values, or success conditions are manufactured or relaxed.

## Source and binary provenance

At build time, the v8 source and the then-current v9 package source were identical:

```text
ab7c3e4412a2bea5c97fee01d640324d329c221c71167bd813b2892a4ffff0f1  v8/package/src/vulkan10_gpu_icd_probe.c
ab7c3e4412a2bea5c97fee01d640324d329c221c71167bd813b2892a4ffff0f1  v9/package/src/vulkan10_gpu_icd_probe.c
13404ed31b936777e722617a3d01c2040b8908cc3dd1afc8c0334a60747140c4  vulkan10_gpu_icd_probe_suballocated.c
c0ddfdbd64a7b60fd97034af88268df7a13c25579fcdd480152a5dbbc23d012b  compute_5500_spv.h (unchanged local copy from v8)
b236748e4c3196070ab7865fcb0d14f083e0d8608adeca0157dcf6c14fd2be40  Vulkan10GpuIcdProbeSuballocated.exe
```

The C source changes are deliberately limited to the suballocated parent-memory allocation/bind/map/synchronization/readback model above, fixture labeling, and the requested `--app-no-portability` spelling for the existing application-side portability opt-out. The copied `compute_5500_spv.h` is byte-identical to v8, and the storage descriptor, shader module, compute pipeline, dispatch, barriers, expected `0x00005500` comparison, and WSI path are otherwise retained.

## Build performed

The checked executable was compiled successfully with the required static Unicode MinGW command:

```bash
cd /home/ubuntu/rimworld-gpu-d3d11-v9/suballocated-compute
x86_64-w64-mingw32-gcc \
  -std=c11 -O2 -Wall -Wextra -Werror -static -municode \
  -I/home/ubuntu/rimworld-vulkan-bridge/build-include \
  -o Vulkan10GpuIcdProbeSuballocated.exe vulkan10_gpu_icd_probe_suballocated.c \
  -luser32 -lkernel32
```

`file` reports `PE32+ executable (console) x86-64, for MS Windows, 19 sections`. Its import table contains only `KERNEL32.dll`, `msvcrt.dll`, and `USER32.dll`; Vulkan is dynamically loaded from the explicit `--loader` argument.

## CLI

Both paths must be absolute Windows paths to existing files. On real target hardware, retain the default Intel-only policy by omitting the sandbox override:

```text
Vulkan10GpuIcdProbeSuballocated.exe \
  --loader C:\absolute\path\vulkan-1.dll \
  --icd C:\absolute\path\ashesD3D11Renderer_icd.json
```

`--app-no-portability` is a **test-fixture option**. It makes the application omit the portability-enumeration/subset opt-in so a compatibility-proxy path can be tested; it changes neither requested Vulkan API version (still 1.0) nor feature requests.

## Exact bounded sandbox command — not run

Run only when no other shared Wine fixture is using this prefix. This command has a **240-second (four-minute) wall-clock limit**. It includes the explicit non-Intel sandbox override and the application-no-portability fixture option. It was **not run** for this build, so there is no runtime result claimed here.

```bash
cd /home/ubuntu/rimworld-gpu-d3d11-v9/suballocated-compute
mkdir -p test-results
timeout 240s xvfb-run -a env \
  WINEPREFIX=/home/ubuntu/rimworld-gpu-d3d11/wine-d3d11-test \
  WINEDLLOVERRIDES='vulkan-1=n;d3dcompiler_47=n;winedbg.exe=d' \
  WINEDEBUG=-all RW_HD5500_MEMORY_TRACE=1 \
  /usr/lib/wine/wine64 /home/ubuntu/rimworld-vulkan-bridge/run-medium.exe \
  'Z:\home\ubuntu\rimworld-gpu-d3d11-v9\suballocated-compute\Vulkan10GpuIcdProbeSuballocated.exe' \
  --loader 'Z:\home\ubuntu\rimworld-gpu-d3d11\test-runtime\vulkan-1.dll' \
  --icd 'Z:\home\ubuntu\rimworld-gpu-d3d11-v9\package\runtime\ashesD3D11Renderer_icd.json' \
  --allow-non-intel-test --app-no-portability \
  > test-results/suballocated-compute.log 2>&1
status=$?
tail -n 80 test-results/suballocated-compute.log
printf 'exit=%s\n' "$status"
```

For a direct portability-opt-in run, omit `--app-no-portability`. For hardware validation, also omit `--allow-non-intel-test` and substitute real target-machine paths. A successful sandbox run would be only a selected synthetic/software-backed integration result, not proof of Intel hardware behavior or RimWorld compatibility.
