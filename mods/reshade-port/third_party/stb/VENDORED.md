# Vendored: stb

Image decoding and resizing for textures declared with a `source` annotation, as ReShade itself
uses them. Compiled only into `src/fx_images.cpp`, with internal linkage where stb allows it.

| File | Upstream | Version | License |
| :-- | :-- | :-- | :-- |
| `stb_image.h` | https://github.com/nothings/stb (master, fetched 2026-10-04) | v2.30 | MIT or public domain (end of file) |
| `stb_image_resize2.h` | same | v2.18 | MIT or public domain (end of file) |
| `stb_image_dds.h` | ReShade v6.8.0 `deps/stb_image/stb_image_dds.h` (a fork of SOIL's DDS loader) | as in ReShade v6.8.0 | public domain (header comment) |

No patches. The binary notice is `res/licenses/STB-LICENSE.txt`.
