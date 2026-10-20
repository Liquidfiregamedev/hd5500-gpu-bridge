#!/usr/bin/env bash
set -euo pipefail
root=/home/ubuntu/rimworld-gpu-d3d11-v13
prefix=/home/ubuntu/jobs/job_qBwr9JZM/wine-prefix-ui-indexed-draw
loader='Z:\home\ubuntu\rimworld-gpu-d3d11-v13\package\compat-loader\rw_system_vulkan.dll'
manifest=${MANIFEST:-'Z:\home\ubuntu\rimworld-gpu-d3d11-v13\package\runtime\ashesD3D11Renderer_icd.json'}
log=${LOG:-"$root/package/tests/ui-indexed-run.log"}
if [[ ! -d $prefix ]]; then
  cp -a /home/ubuntu/rimworld-gpu-d3d11/wine-d3d11-test "$prefix"
fi
rm -f /tmp/.X161-lock
Xvfb :161 -screen 0 1024x768x24 -nolisten tcp >"$prefix/xvfb161-ui-indexed.log" 2>&1 &
xvfb=$!
trap 'kill "$xvfb" 2>/dev/null || true; wait "$xvfb" 2>/dev/null || true' EXIT
timeout 90s env DISPLAY=:161 WINEPREFIX="$prefix" WINEDEBUG=-all \
  RW_HD5500_FRAME_PROBE=1 WINEDLLOVERRIDES='vulkan-1=n;d3dcompiler_47=n;winedbg.exe=d' \
  /usr/lib/wine/wine64 /home/ubuntu/rimworld-vulkan-bridge/run-medium.exe \
  'Z:\home\ubuntu\rimworld-gpu-d3d11-v13\package\tests\UiIndexedDrawProbe.exe' \
  --loader "$loader" --icd "$manifest" --allow-non-intel-test --require-frame-probe \
  >"$log" 2>&1
