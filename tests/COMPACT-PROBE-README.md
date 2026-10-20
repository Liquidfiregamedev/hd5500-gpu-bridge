# Compact Parent-Backing Raw-Vulkan Probe

`compact_parent_bind_probe.exe` is a **standalone x64 Windows raw-Vulkan fixture** for the compact native-backing candidate. It is derived in intent from the v8 staging bind repro, but is isolated here and does not use VMA, the game, or renderer code.

It dynamically loads only the `--loader` passed on its command line. Before doing so, it sets both `VK_ICD_FILENAMES` and `VK_DRIVER_FILES` **in its own process only** to the absolute `--icd` path. It imports no Vulkan entry points from its executable import table.

## Test contract

| Item | Exact behavior |
|---|---|
| Parent backing | One **exactly 16 MiB** `VkDeviceMemory`, selected from a memory type with both `HOST_VISIBLE` and `HOST_COHERENT` |
| Simultaneous children | Eight live `VkBuffer` objects, each created with a logical size of **4 KiB** |
| Bind layout | Every child binds to that one parent at a distinct, aligned, **non-zero** offset: 1 MiB through 8 MiB (1 MiB stride). It refuses a device whose reported requirements cannot safely fit this layout. |
| Default mode | `TRANSFER_SRC | TRANSFER_DST`; no compute pipeline or dispatch |
| `--storage` mode | Adds `STORAGE_BUFFER` to every child buffer. It still creates **no compute work**. It additionally creates one intentionally unbound storage sentinel to make its legal cleanup order visible. |
| Initial data path | Maps the parent, writes a unique pattern for every child plus a distinct guard pattern across the rest of all 16 MiB, explicitly flushes, and unmaps. |
| Copy under test | Records **last child → first child**, exactly **257 bytes**, source offset **13**, destination offset **29**. |
| Data criterion | After `vkDeviceWaitIdle`, maps and invalidates the parent, then compares the entire **16 MiB byte-for-byte**. This proves the exact destination bytes and that all non-destination bytes—including source, neighboring children, stride gaps, and outer guard regions—are unchanged. |
| Release/reuse criterion | Destroys child 1 while retaining its parent allocation; creates a new 4 KiB child and binds it at the released 2 MiB offset; writes a new unique pattern, copies it to child 0, waits idle, invalidates, and performs another full 16 MiB exact comparison. |
| Lifetime cleanup | Destroys all parent-bound child buffers before freeing the parent. In `--storage` mode the separate, unbound storage sentinel is destroyed **after** parent free; this is deliberately legal and makes the parent-first-versus-unrelated-buffer cleanup case observable. |

The fixture deliberately **does not include a GPU-only-source mode**. The narrow target is parent-host mapping, flush/invalidate propagation, and child-at-offset backing reuse. Introducing a staging allocation/GPU-only source would change that backing model and weaken the focused diagnosis.

## CLI

```text
compact_parent_bind_probe.exe \
  --loader C:\absolute\vulkan-1.dll \
  --icd C:\absolute\ashesD3D11Renderer_icd.json \
  [--storage] [--allow-non-intel-test]
```

Both path arguments are mandatory absolute Windows paths and must already exist. By default, selection is **Intel vendor ID `0x8086` and non-software only**. `--allow-non-intel-test` is an explicit sandbox control: it permits a Wine/synthetic adapter, prints a warning, and is **not Intel hardware validation**. The executable never supplies that flag itself.

## Build

From this directory:

```bash
x86_64-w64-mingw32-gcc -std=c11 -O2 -static -municode \
  -I/home/ubuntu/rimworld-vulkan-bridge/build-include \
  -o compact_parent_bind_probe.exe compact_parent_bind_probe.c
file compact_parent_bind_probe.exe
```

Expected file classification: `PE32+ executable (console) x86-64, for MS Windows`.

## Known-wrapper sandbox invocation

Run tests **sequentially** and only when the shared prefix is idle. The command below is deliberately wall-clock bounded to 300 seconds (under six minutes). It uses the requested native loader and known `run-medium.exe` wrapper. The `--allow-non-intel-test` flag is present only in this sandbox command.

The following is the concrete v9-candidate invocation. It remains a sandbox-integration command and does **not** claim Intel or full-game validation:

```bash
cd /home/ubuntu/rimworld-gpu-d3d11-v9/compact-probe
mkdir -p test-results
timeout 300s xvfb-run -a env \
  WINEPREFIX=/home/ubuntu/rimworld-gpu-d3d11/wine-d3d11-test \
  WINEDLLOVERRIDES='vulkan-1=n;d3dcompiler_47=n;winedbg.exe=d' \
  WINEDEBUG=-all RW_HD5500_MEMORY_TRACE=1 \
  /usr/lib/wine/wine64 /home/ubuntu/rimworld-vulkan-bridge/run-medium.exe \
  'Z:\home\ubuntu\rimworld-gpu-d3d11-v9\compact-probe\compact_parent_bind_probe.exe' \
  --loader 'Z:\home\ubuntu\rimworld-gpu-d3d11\test-runtime\vulkan-1.dll' \
  --icd 'Z:\home\ubuntu\rimworld-gpu-d3d11-v9\package\runtime\ashesD3D11Renderer_icd.json' \
  --allow-non-intel-test \
  > test-results/compact-parent-transfer.log 2>&1
status=$?
tail -n 60 test-results/compact-parent-transfer.log
printf 'exit=%s\n' "$status"
```

If the candidate manifest is rebuilt or relocated, keep every argument and environment setting above unchanged **except** replace only `--icd` with its new absolute Windows manifest path. For the storage-UAV usage variant, add `--storage` before the final redirection:

```bash
cd /home/ubuntu/rimworld-gpu-d3d11-v9/compact-probe
mkdir -p test-results
timeout 300s xvfb-run -a env \
  WINEPREFIX=/home/ubuntu/rimworld-gpu-d3d11/wine-d3d11-test \
  WINEDLLOVERRIDES='vulkan-1=n;d3dcompiler_47=n;winedbg.exe=d' \
  WINEDEBUG=-all RW_HD5500_MEMORY_TRACE=1 \
  /usr/lib/wine/wine64 /home/ubuntu/rimworld-vulkan-bridge/run-medium.exe \
  'Z:\home\ubuntu\rimworld-gpu-d3d11-v9\compact-probe\compact_parent_bind_probe.exe' \
  --loader 'Z:\home\ubuntu\rimworld-gpu-d3d11\test-runtime\vulkan-1.dll' \
  --icd 'Z:\home\ubuntu\rimworld-gpu-d3d11-v9\package\runtime\ashesD3D11Renderer_icd.json' \
  --allow-non-intel-test --storage \
  > test-results/compact-parent-storage.log 2>&1
status=$?
tail -n 60 test-results/compact-parent-storage.log
printf 'exit=%s\n' "$status"
```

For real hardware validation, omit `--allow-non-intel-test` and use actual absolute Windows paths on the target machine. A successful Wine/Xvfb run means only that the fixture passed against the selected synthetic/software-backed sandbox adapter and ICD; it is neither Intel/HD 5500 proof nor full-game proof.

## Log contract

An exit status of zero is credible only with all of the following in the same log:

```text
PASS: all eight buffers are simultaneously live and bound to the single parent allocation.
RESULT: initial subset copy last->first (source+13, destination+29, 257 bytes) => VK_SUCCESS (0)
RESULT: vkDeviceWaitIdle after initial subset copy => VK_SUCCESS (0)
PASS: INITIAL READBACK (257-byte destination and unchanged neighbors) exact 16 MiB parent comparison passed ...
RESULT: vkBindBufferMemory(recycled child, retained parent, released offset) => VK_SUCCESS (0)
PASS: RECYCLED-SLOT READBACK (released child, retained parent, reused offset) exact 16 MiB parent comparison passed ...
PASS: compact parent-backing fixture completed: bind, subset-copy integrity, neighbor integrity, and release/reuse lifecycle all passed.
```

`FINAL: FAIL`, a nonzero process exit, a timeout, a missing initial/readback pass line, or any `MISMATCH:` line is a failure. The fixture does not reduce the byte-exact comparison to samples and does not manufacture handles, feature support, or success records.
