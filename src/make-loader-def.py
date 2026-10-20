#!/usr/bin/env python3
"""Generate exports from the actual loader, not a software Vulkan ICD."""
import sys
from pathlib import Path
import pefile
if len(sys.argv) != 3:
    raise SystemExit('usage: make-loader-def.py genuine-vulkan-1.dll output.def')
pe = pefile.PE(sys.argv[1])
wrapped = {'vkCreateInstance', 'vkCreateDevice', 'vkGetInstanceProcAddr', 'vkGetDeviceProcAddr'}
exports = [symbol.name.decode('ascii') for symbol in pe.DIRECTORY_ENTRY_EXPORT.symbols if symbol.name]
if not wrapped.issubset(exports):
    raise SystemExit('The input is not a suitable full Vulkan loader.')
lines = ['LIBRARY vulkan-1', 'EXPORTS']
for name in sorted(set(exports)):
    if not name.startswith('vk'):
        raise SystemExit(f'Unexpected non-Vulkan export: {name}')
    lines.append('  ' + name if name in wrapped else f'  {name}=rw_system_vulkan.{name}')
Path(sys.argv[2]).write_text('\n'.join(lines)+'\n', encoding='ascii')
print(f'Export table: {len(set(exports))} genuine loader exports, {len(wrapped)} wrapped.')
