#!/usr/bin/env bash
# HD5500 bridge emulator regression suite: runs every smoke mode and compares the frame-3
# swapchain mean colour against known-good values. Exit 1 on any mismatch.
# Usage: run-suite.sh [path/to/ashesD3D11Renderer.dll]
set -u
RUN=${RUN:-$HOME/wt/run}
DLL=${1:-$RUN/ashesD3D11Renderer.dll}
cp -f "$DLL" "$RUN/suite-under-test.dll"
export WINEDLLOVERRIDES="d3dcompiler_47=n"   # Microsoft HLSL compiler from the Windows SDK, copied into $RUN
declare -A EXPECT=(
  [x]="209.1,45.9,0.0"        [composite]="41.8,87.7,169.8"  [push]="197.9,57.1,0.0"
  [loadpass]="209.1,45.9,0.0" [clearattach]="104.6,45.9,104.6" [hdrresolve]="209.1,45.9,0.0"
  [mipblit]="128.0,0.0,0.0"   [swizzle]="41.8,41.8,169.8"    [ssbo]="244.8,15.4,10.2"
  [pushubo]="255.0,8.0,8.0"  [ssbowrite]="209.1,45.9,0.0"  [areaclear]="104.6,22.9,127.5"  [dynubo]="247.0,8.0,8.0"  [vbopersist]="209.1,0.0,45.9"  [varying]="209.1,0.0,45.9"  [varyblock]="209.1,0.0,45.9"  [varystruct]="209.1,0.0,45.9"  [instuav]="232.0,0.0,22.9"  [uavlayout]="232.0,0.0,22.9"  [uavoffset]="232.0,0.0,22.9"  [uavdyn]="232.0,0.0,22.9"  [uavalias]="232.0,0.0,22.9"
)
fail=0
cd "$RUN"
for m in x composite push loadpass clearattach hdrresolve mipblit swizzle ssbo pushubo ssbowrite areaclear dynubo vbopersist varying varyblock varystruct instuav uavlayout uavoffset uavdyn uavalias; do
  RW_HD5500_FRAME_DIAG=1 timeout 120 xvfb-run -a wine smoke.exe 'Z:\root\wt\run\suite-under-test.dll' 4 "$m" > "suite-$m.txt" 2>&1
  got=$(grep -E 'dump frame=3 tag=swapchain' "suite-$m.txt" | grep -o 'meanRGB=[^ ]*' | cut -d= -f2)
  pass=$(grep -c '^PASS' "suite-$m.txt")
  if [ "$pass" = 1 ] && [ "$got" = "${EXPECT[$m]}" ]; then printf 'ok    %-12s %s\n' "$m" "$got"
  else printf 'FAIL  %-12s got=%s expected=%s\n' "$m" "${got:-none}" "${EXPECT[$m]}"; fail=1; fi
done
# dynamic UBO through the feature-level-11_0 copy path (RW_GPU_CB_EMULATE=1)
RW_GPU_CB_EMULATE=1 RW_HD5500_FRAME_DIAG=1 timeout 120 xvfb-run -a wine smoke.exe 'Z:\root\wt\run\suite-under-test.dll' 4 dynubo > suite-dynubo-copy.txt 2>&1
got=$(grep -E 'dump frame=3 tag=swapchain' suite-dynubo-copy.txt | grep -o 'meanRGB=[^ ]*' | cut -d= -f2)
if [ "$got" = "247.0,8.0,8.0" ] && grep -q RW-GPU-CBWINDOW suite-dynubo-copy.txt; then echo "ok    dynubo-copy  $got"; else echo "FAIL  dynubo-copy  got=${got:-none}"; fail=1; fi
# v13.48 write-watch: every shadow tracked (threshold 1 byte), frame 2 rewrites the dynamic UBO
# in place; only the dirty page may be copied and the new colour must still arrive.
RW_GPU_WRITE_WATCH_MIN=1 RW_HD5500_FRAME_DIAG=1 timeout 120 xvfb-run -a wine smoke.exe 'Z:\root\wt\run\suite-under-test.dll' 4 dynubo > suite-dynubo-watch.txt 2>&1
got=$(grep -E 'dump frame=3 tag=swapchain' suite-dynubo-watch.txt | grep -o 'meanRGB=[^ ]*' | cut -d= -f2)
if [ "$got" = "247.0,8.0,8.0" ]; then echo "ok    dynubo-watch $got"; else echo "FAIL  dynubo-watch got=${got:-none}"; fail=1; fi
# v13.49 api11: Vulkan 1.1 entry points when the bridge reports 1.1 (RW_GPU_API_VERSION=1.1)
SMOKE_API11=1 RW_GPU_API_VERSION=1.1 RW_HD5500_FRAME_DIAG=0 timeout 120 xvfb-run -a wine smoke.exe 'Z:\root\wt\run\suite-under-test.dll' 2 x > suite-api11.txt 2>&1
if grep -q 'api11-check PASS' suite-api11.txt; then echo "ok    api11        1.1 entry points answer correctly"; else echo "FAIL  api11        $(grep api11-check suite-api11.txt | tr '\n' ' ')"; fail=1; fi
# watchdog self-test: badblit must fail recording and print the watchdog
# v13.60b: swapchain recreated with oldSwapchain (window resize) -> both must be flip-model, picture correct
SMOKE_RECREATE=1 RW_HD5500_FRAME_DIAG=1 timeout 120 xvfb-run -a wine smoke.exe 'Z:\root\wt\run\suite-under-test.dll' 4 x > suite-recreate.txt 2>&1
if grep -q '^PASS' suite-recreate.txt && grep -q 'swapchain recreated' suite-recreate.txt && [ "$(grep -c 'presentation model flip-discard' suite-recreate.txt)" -ge 2 ] && ! grep -q 'flip-model swapchain refused' suite-recreate.txt; then printf 'ok    %-12s %s\n' recreate "flip-model swapchain recreated"; else printf 'FAIL  %-12s\n' recreate; fail=1; fi
RW_HD5500_FRAME_DIAG=0 timeout 120 xvfb-run -a wine smoke.exe 'Z:\root\wt\run\suite-under-test.dll' 2 badblit > suite-badblit.txt 2>&1
if grep -q 'RW-GPU-WATCHDOG' suite-badblit.txt; then echo "ok    badblit      watchdog fired"; else echo "FAIL  badblit      watchdog silent"; fail=1; fi
# v13.41 water: RimWorld's two-pass water with the game's own shaders, compared pixel-by-pixel
# (8x6 sample grid read back by smoke.exe itself) against the reference ICD (Windows lavapipe,
# vulkan_lvp.dll in $RUN, assembled from package/reference/lavapipe). Tolerance: mean abs
# channel difference <= 4 and at most 4 samples differing by more than 12 (edge pixels).
RW_HD5500_FRAME_DIAG=1 timeout 180 xvfb-run -a wine smoke.exe 'Z:\root\wt\run\suite-under-test.dll' 4 water > suite-water.txt 2>&1
# v13.49: the same water test with SPIR-V 1.3 shaders (what Waylith sends when the device reports
# Vulkan 1.1+), bridge reporting 1.1; must give the identical picture (compared below too).
RW_GPU_API_VERSION=1.1 RW_HD5500_FRAME_DIAG=1 timeout 180 xvfb-run -a wine smoke13.exe 'Z:\root\wt\run\suite-under-test.dll' 4 water > suite-water13.txt 2>&1
if [ -f vulkan_lvp.dll ]; then
  [ -f suite-water-reference.txt ] && grep -q '^readback grid' suite-water-reference.txt || RW_HD5500_FRAME_DIAG=0 timeout 300 xvfb-run -a wine smoke.exe 'Z:\root\wt\run\vulkan_lvp.dll' 4 water > suite-water-reference.txt 2>&1
  verdict=$(python3 - <<'PY'
import re
def grid(f):
    for l in open(f, errors='replace'):
        if l.startswith('readback grid:'):
            return [tuple(int(c) for c in p.split(',')) for p in l.split()[2:]]
    return None
a=grid('suite-water.txt'); b=grid('suite-water-reference.txt')
sd=None
for l in open('suite-water.txt', errors='replace'):
    m=re.search(r'lumaStddev=([0-9.]+)', l)
    if m: sd=float(m.group(1))
if not a or not b or len(a)!=len(b): print('FAIL no-grid'); raise SystemExit
d=[max(abs(x-y) for x,y in zip(p,q)) for p,q in zip(a,b)]
mean=sum(d)/len(d); big=sum(1 for v in d if v>12)
c=grid('suite-water13.txt')
same13 = c is not None and c == a
print(('ok' if mean<=4 and big<=4 and (sd or 0)>10 and same13 else 'FAIL')+' meanDiff=%.1f maxDiff=%d big=%d lumaStddev=%s spirv13=%s'%(mean,max(d),big,sd,'identical' if same13 else 'DIFFERENT'))
PY
)
  case "$verdict" in ok*) echo "ok    water        $verdict";; *) echo "FAIL  water        $verdict"; fail=1;; esac
else
  got=$(grep -E 'dump frame=3 tag=swapchain' suite-water.txt | grep -o 'meanRGB=[^ ]*' | cut -d= -f2)
  if grep -q '^PASS' suite-water.txt && [ -n "$got" ]; then echo "ok    water        $got (no reference ICD)"; else echo "FAIL  water        got=${got:-none}"; fail=1; fi
fi
# v13.66 ibofs: the water picture again with the index buffer bound at non-zero offsets behind
# wrong indices, split into three direct draws (SMOKE_IBOFS=2), and once more with an indexed
# indirect draw at index offset 0 (SMOKE_IBOFS=3); both must equal the normal water grid.
# (Indirect + non-zero index offset also fails with the bridge's folding switched off and passes
# on lavapipe: Wine's D3D11 ignores the index-buffer offset in DrawIndexedInstancedIndirect;
# not testable in this environment.)
ok=1; wa=$(grep -h 'readback grid:' suite-water.txt | head -1)
for m in 2 3; do
  SMOKE_IBOFS=$m RW_HD5500_FRAME_DIAG=1 timeout 180 xvfb-run -a wine smoke.exe 'Z:\root\wt\run\suite-under-test.dll' 4 water > suite-ibofs$m.txt 2>&1
  ia=$(grep -h 'readback grid:' suite-ibofs$m.txt | head -1)
  { [ -n "$ia" ] && [ "$ia" = "$wa" ] && grep -q '^PASS' suite-ibofs$m.txt; } || ok=0
done
if [ $ok = 1 ]; then printf 'ok    %-12s %s\n' ibofs "offset and indirect index draws identical to the normal water picture"; else printf 'FAIL  %-12s\n' ibofs; fail=1; fi
# v13.73 setcopy: descriptor-set content fast path. =2 second half drawn with a new set handle of
# identical content, =3 the same with a set of different textures bound in between: both must equal
# the normal water picture; =4 second half with the different set: must differ (fast path must not fire).
ok=1
for m in 2 3; do
  SMOKE_SETCOPY=$m RW_HD5500_FRAME_DIAG=1 timeout 180 xvfb-run -a wine smoke.exe 'Z:\root\wt\run\suite-under-test.dll' 4 water > suite-setcopy$m.txt 2>&1
  ia=$(grep -h 'readback grid:' suite-setcopy$m.txt | head -1)
  { [ -n "$ia" ] && [ "$ia" = "$wa" ] && grep -q '^PASS' suite-setcopy$m.txt; } || ok=0
done
SMOKE_SETCOPY=4 RW_HD5500_FRAME_DIAG=1 timeout 180 xvfb-run -a wine smoke.exe 'Z:\root\wt\run\suite-under-test.dll' 4 water > suite-setcopy4.txt 2>&1
ia=$(grep -h 'readback grid:' suite-setcopy4.txt | head -1)
{ [ -n "$ia" ] && [ "$ia" != "$wa" ]; } || ok=0
if [ $ok = 1 ]; then printf 'ok    %-12s %s\n' setcopy "same-content set handles draw identically; different content draws differently"; else printf 'FAIL  %-12s\n' setcopy; fail=1; fi
# v13.73 multiwin: three windows of one persistently mapped uniform buffer per frame, new colours
# every frame; the merged upload must give the same picture as one upload per window, and the last
# frame's colours (window 0 = 255,0,64; window 768 = 0,64,255) must be visible.
ok=1
for e in 1 0; do
  RW_GPU_UNIFORM_RING=0 RW_GPU_UPLOAD_MERGE=$e SMOKE_MULTIWIN=1 RW_HD5500_FRAME_DIAG=1 timeout 120 xvfb-run -a wine smoke.exe 'Z:\root\wt\run\suite-under-test.dll' 5 dynubo > suite-multiwin$e.txt 2>&1
  grep -q '^PASS' suite-multiwin$e.txt || ok=0
done
m1=$(grep -h 'readback grid:' suite-multiwin1.txt | head -1); m0=$(grep -h 'readback grid:' suite-multiwin0.txt | head -1)
{ [ -n "$m1" ] && [ "$m1" = "$m0" ] && echo "$m1" | grep -q ' 255,0,64' && echo "$m1" | grep -q ' 0,64,255' && grep -q 'RW-GPU-UPLOAD\] merging' suite-multiwin1.txt; } || ok=0
if [ $ok = 1 ]; then printf 'ok    %-12s %s\n' multiwin "merged window uploads identical to one upload per window, newest data"; else printf 'FAIL  %-12s\n' multiwin; fail=1; fi
# v13.74 ubring: the same three-window picture with the uniform ring on (default), off, with a tiny
# ring that wraps (DISCARD) every frame, and with a ring too small for the batch (fallback path):
# all four identical to the merged-upload picture above, with the newest colours.
ok=1
for v in on off wrap small; do
  case $v in on) E="RW_GPU_UNIFORM_RING=1";; off) E="RW_GPU_UNIFORM_RING=0";; wrap) E="RW_GPU_UNIFORM_RING=1 RW_GPU_UNIFORM_RING_TEST_BYTES=1024";; small) E="RW_GPU_UNIFORM_RING=1 RW_GPU_UNIFORM_RING_TEST_BYTES=512";; esac
  env $E SMOKE_MULTIWIN=1 RW_HD5500_FRAME_DIAG=1 timeout 120 xvfb-run -a wine smoke.exe 'Z:\root\wt\run\suite-under-test.dll' 5 dynubo > suite-ubring-$v.txt 2>&1
  g=$(grep -h 'readback grid:' suite-ubring-$v.txt | head -1)
  { grep -q '^PASS' suite-ubring-$v.txt && [ -n "$g" ] && [ "$g" = "$m1" ]; } || ok=0
done
grep -q 'RW-GPU-UBRING\] first batch' suite-ubring-on.txt || ok=0
grep -q 'RW-GPU-UBRING\] first batch' suite-ubring-off.txt && ok=0
if [ $ok = 1 ]; then printf 'ok    %-12s %s\n' ubring "uniform ring on/off/wrapping/too small: identical pictures"; else printf 'FAIL  %-12s\n' ubring; fail=1; fi
# v13.74 idxpool: the water pass drawn from two extra index buffers (one host-mapped, one filled by
# vkCmdCopyBuffer each frame), wrong indices in frame 1 and right ones later. With the index pool on,
# off, too small (restarts until it switches itself off) and smaller than one buffer: all identical to
# the normal water picture; a 1-frame run (wrong indices) must differ (the test can see stale data).
ok=1
for v in on off restart tiny; do
  case $v in on) E="RW_GPU_INDEX_POOL=1";; off) E="RW_GPU_INDEX_POOL=0";; restart) E="RW_GPU_INDEX_POOL=1 RW_GPU_INDEX_POOL_TEST_BYTES=1024";; tiny) E="RW_GPU_INDEX_POOL=1 RW_GPU_INDEX_POOL_TEST_BYTES=512";; esac
  env $E SMOKE_IDXMULTI=1 RW_HD5500_FRAME_DIAG=1 timeout 180 xvfb-run -a wine smoke.exe 'Z:\root\wt\run\suite-under-test.dll' 4 water > suite-idxpool-$v.txt 2>&1
  g=$(grep -h 'readback grid:' suite-idxpool-$v.txt | head -1)
  { grep -q '^PASS' suite-idxpool-$v.txt && [ -n "$g" ] && [ "$g" = "$wa" ]; } || ok=0
done
grep -q 'IDXPOOL\] first index buffer copied' suite-idxpool-on.txt || ok=0
grep -q 'switched off for this session' suite-idxpool-restart.txt || ok=0
SMOKE_IDXMULTI=1 RW_HD5500_FRAME_DIAG=1 timeout 180 xvfb-run -a wine smoke.exe 'Z:\root\wt\run\suite-under-test.dll' 1 water > suite-idxpool-f1.txt 2>&1
g=$(grep -h 'readback grid:' suite-idxpool-f1.txt | head -1); { [ -n "$g" ] && [ "$g" != "$wa" ]; } || ok=0
if [ $ok = 1 ]; then printf 'ok    %-12s %s\n' idxpool "index pool on/off/restarting/too small: identical; stale indices would be visible"; else printf 'FAIL  %-12s\n' idxpool; fail=1; fi
# v13.75 ubowin: the water pass from two new sets that differ only in the uniform-buffer offset (the
# second window tints the water red) - Waylith's per-draw pattern. Content fast path + uniform ring must
# give the same picture as the old path, the tint must be visible, and the fast path must have fired.
ok=1
RW_GPU_SET_CONTENT_FAST=1 RW_GPU_SET_CONTENT_UBO=1 RW_GPU_UNIFORM_RING=1 SMOKE_UBOWIN=1 RW_HD5500_FRAME_DIAG=1 timeout 180 xvfb-run -a wine smoke.exe 'Z:\root\wt\run\suite-under-test.dll' 4 water > suite-ubowin-new.txt 2>&1
RW_GPU_SET_CONTENT_UBO=0 RW_GPU_UNIFORM_RING=0 SMOKE_UBOWIN=1 RW_HD5500_FRAME_DIAG=1 timeout 180 xvfb-run -a wine smoke.exe 'Z:\root\wt\run\suite-under-test.dll' 4 water > suite-ubowin-old.txt 2>&1
gn=$(grep -h 'readback grid:' suite-ubowin-new.txt | head -1); go=$(grep -h 'readback grid:' suite-ubowin-old.txt | head -1)
{ grep -q '^PASS' suite-ubowin-new.txt && grep -q '^PASS' suite-ubowin-old.txt && [ -n "$gn" ] && [ "$gn" = "$go" ] && [ "$gn" != "$wa" ]; } || ok=0
grep -q 'RW-GPU-SET\] descriptor-set content fast path in use' suite-ubowin-new.txt || ok=0
grep -q 'RW-GPU-UBRING\] first batch' suite-ubowin-new.txt || ok=0
if [ $ok = 1 ]; then printf 'ok    %-12s %s\n' ubowin "sets differing only in uniform offset: fast path + ring identical to old path"; else printf 'FAIL  %-12s\n' ubowin; fail=1; fi
[ $fail = 0 ] && echo "SUITE PASS" || echo "SUITE FAIL"
exit $fail
