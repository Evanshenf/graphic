# graphic: LS7A2000 display controller

Authoritative source and build directory: `/home/virgo/graphic` on the lab board.
The original 28 files are preserved by commit
`a483dd089dfe5520fafb786568427364601f373d` and tag `graphic-original`.
Development continues on branch `graphic-rewrite`. The local checkout is a
synchronized editing/evidence copy, not a replacement project mainline.

## Structure

- `lsdc_module`, `lsdc_pci`, `lsdc_driver`: registration, PCI and DRM lifetime.
- `soc/lsdc_gmch`: paired GPU VRAM ownership and board PIX PLL mapping.
- `lsdc_gem`, `lsdc_region_ttm`, `gem/lsdc_gem_ttm`: zeroed dumb buffers,
  SYSTEM/VRAM placement, mapping and scanout pins.
- `display/lsdc_display_driver`, `lsdc_plane`, `lsdc_crtc`, `lsdc_output`:
  atomic KMS, primary/cursor planes, flip completion and HDMI handoff.

Hardware programming is based on the official LS7A2000 user manual V1.3.
Only generic DRM/TTM/PCI interfaces are used as kernel implementation references.
Existing upstream/vendor Loongson and lg110 implementations are excluded.

## Initial support and limits

The first version preserves each output's configured firmware timing and PLL
parameters. The connected lab HDMI1 output has 1920x1080 timing, a measured PLL
clock of 148485 kHz, and an inherited PLL VCO of 1633.33 MHz. The VCO is below the
manual's 1.8 GHz minimum. Preserving a previously locked configuration is a
handoff policy, not evidence that those parameters satisfy the manual.
No pixel or PHY PLL retuning, EDID/DDC, audio configuration, suspend/resume or
GPU command submission is implemented. The manual lacks the required HDMI PHY
frequency formula and DDC wiring. HDMI0 mirrors VGA in hardware; this version
exposes HDMI connectors and does not claim independent VGA detection.

Primary scanout is linear XRGB8888 with stride aligned to 256 bytes. CPU VRAM
resource addresses and PCI bus scanout addresses are kept separate; no
undocumented framebuffer high-address register is written. The first cursor
format is packed 64x64 ARGB8888, 256-byte pitch. Negative positions use the
manual's hot-spot fields and require visual confirmation. No 32x32 cursor stride
conversion or offloaded 2D/3D rendering is claimed.

`bridge_base` defaults to the lab board's BIOS-selected 0x10010000 bridge
configuration aperture; it is not a universal PCI discovery rule.
A conservative 20 ms drain after output disable is an engineering choice:
the manual supplies no DMA-idle completion bit.

## Build and smoke test

The active target kernel is `7.3.0-rc3-gpulab-bca45af`. DRM core/KMS helpers are
built into this kernel; `ttm` and `drm_ttm_helper` remain separate modules.
The new hardware module is `graphic.ko`.

```sh
cd /home/virgo/graphic
make W=1 -j8
cc -std=c11 -O2 -Wall -Wextra -Werror tests/kms-smoke.c \
  -o /tmp/graphic-kms-smoke $(pkg-config --cflags --libs libdrm)
```

The smoke test needs exclusive KMS ownership. It checks zero initialization,
map/readback, modeset, vblank, ten page flips and 64x64 cursor ioctls before
restoring the previous CRTC. These software checks do not prove the monitor's
pixel colors, alpha appearance or edge clipping; those need visual observation.
Initial live verification passed on 2026-10-04: three BOs were fully zero,
1080p modeset and hardware vblank succeeded, ten bank-checked flips completed
in 16-17 ms each, cursor ioctls at (100,100) and (-16,-16) passed, and ordinary
unload/reload restored LightDM. The user confirmed normal display and pointer.
Detailed results are in the lab evidence directory. This does not establish
untested modes, 32x32 cursor layout, dual-monitor operation or acceleration.
