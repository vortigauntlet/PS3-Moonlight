# tiny3d (patched copy of `tiny3d.c`)

This folder is a copy of `lib/source/tiny3d.c` and the headers it needs, from
**Tiny3D 2.0** by Hermes (`ps3dev/ps3libraries`, the version the PS3 toolchain
installs as `libtiny3d.a`). Tiny3D states it uses the same licence as PSL1GHT
(MIT); see its README.

## The one change

In `nv_shaders.h`, the planar YUV fragment programs `nv30_fp_yuv8` and
`nv30_fp_yuv_color8` add the BT.601 green terms where they should subtract
them:

| constant | upstream | here |
|---|---|---|
| Cr→G | `0x3f5020c5` (+0.813) | `0xbf5020c5` (−0.813) |
| Cb→G | `0x3ec83127` (+0.391) | `0xbec83127` (−0.391) |

With the upstream values, reds come out gold and skin tones blue. This was
measured on a real PS3 by capturing the framebuffer with four different chroma
bindings and fitting the shader's effective matrix: red and blue matched BT.601
exactly, and green had both signs flipped. Nothing else differs from upstream,
and the 32-bit AYUV shaders are left as they were.

## How it is linked

The Makefile compiles this `tiny3d.c` with tiny3d's own flags and lists it
first in `OFILES`. Every symbol `tiny3d.o` defines is then already resolved, so
the linker never pulls the unpatched `tiny3d.o` out of `libtiny3d.a`. The rest
of the library (`rsxutil.o`, `matrix.o`, …) still comes from the archive.
