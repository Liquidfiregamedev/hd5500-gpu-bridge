#!/usr/bin/env bash
# Rebuild the Windows x64 probe. Requires glslangValidator, spirv-val, Python 3,
# and x86_64-w64-mingw32-gcc. This script does not download or modify any SDK.
set -euo pipefail
cd "$(dirname "$0")"

HEADER_ROOT="${VULKAN_HEADERS:-/home/ubuntu/rimworld-vulkan-bridge/build-include}"
test -f "$HEADER_ROOT/vulkan/vulkan.h" || {
  echo "ERROR: Vulkan header not found: $HEADER_ROOT/vulkan/vulkan.h" >&2
  exit 2
}

glslangValidator -V --target-env vulkan1.0 -S comp compute_5500.comp -o compute_5500.spv
spirv-val --target-env vulkan1.0 compute_5500.spv
python3 - <<'PY'
from pathlib import Path
p = Path('compute_5500.spv')
b = p.read_bytes()
if len(b) % 4 or b[:4] != b'\x03\x02#\x07':
    raise SystemExit('ERROR: expected little-endian SPIR-V binary')
words = [int.from_bytes(b[i:i + 4], 'little') for i in range(0, len(b), 4)]
with open('compute_5500_spv.h', 'w', newline='\n') as out:
    out.write('/* Generated from compute_5500.spv; Vulkan 1.0 / SPIR-V 1.0. */\n')
    out.write('static const uint32_t probe_compute_5500_spv[] = {\n')
    for i, word in enumerate(words):
        out.write(f'  0x{word:08x}u,' + ('\n' if (i + 1) % 6 == 0 else ' '))
    out.write('\n};\n')
    out.write('static const size_t probe_compute_5500_spv_size = sizeof(probe_compute_5500_spv);\n')
PY

x86_64-w64-mingw32-gcc \
  -std=c11 -O2 -Wall -Wextra -Werror -static -municode \
  -I"$HEADER_ROOT" vulkan10_gpu_icd_probe.c \
  -o Vulkan10GpuIcdProbe.exe -luser32 -lkernel32
file Vulkan10GpuIcdProbe.exe
