#!/usr/bin/env python3
"""Check the driver layer boundaries, without compiling or touching hardware."""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
errors = []


def code(path):
    text = path.read_text()
    return re.sub(r"/\*.*?\*/|//[^\n]*", "", text, flags=re.S)


for path in [*ROOT.glob("*.c"), *ROOT.glob("*.h"),
             *(ROOT / "memory").glob("*.[ch]"),
             *(ROOT / "display").glob("*.[ch]")]:
    source = code(path)
    if re.search(r"\b(readl|writel|ioread32|iowrite32)\s*\(", source):
        errors.append(f"MMIO escaped the hardware backend: {path.relative_to(ROOT)}")
    if re.search(r"#include.*_regs\.h", source):
        errors.append(f"Private register header escaped: {path.relative_to(ROOT)}")

for path in (ROOT / "hardware").glob("*.[ch]"):
    if re.search(r"#include.*(drm/|graphic_device|graphic_display)", code(path)):
        errors.append(f"Hardware backend depends on DRM state: {path.name}")

for path in (ROOT / "display").glob("*.[ch]"):
    if re.search(r"#include.*drm/ttm/|\bttm_(bo|device)_", code(path)):
        errors.append(f"TTM internals escaped the memory layer: {path.name}")

if re.search(r"drm/(drm_plane|drm_crtc|drm_connector|ttm/)",
             code(ROOT / "graphic_device.h")):
    errors.append("The device header exposes private display/TTM objects")

makefile = (ROOT / "Makefile").read_text()
targets = set(re.findall(r"obj-m\s*\+=\s*([A-Za-z0-9_/]+\.o)", makefile))
objects = [obj for obj in re.findall(r"[A-Za-z0-9_/]+\.o\b", makefile)
           if obj not in targets]
for obj in objects:
    if not (ROOT / obj.replace(".o", ".c")).is_file():
        errors.append(f"Kbuild object has no source: {obj}")

for error in errors:
    print("FAIL", error)
if errors:
    raise SystemExit(1)
print(f"PASS architecture boundaries; {len(objects)} Kbuild objects")
