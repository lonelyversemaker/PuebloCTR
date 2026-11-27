# Third-party parts

| Part | Where | Licence |
|---|---|---|
| CubeEvil4 (the decompilation-based port PuebloCTR is built on; a fork of the [decompilation by adonis-singh](https://github.com/adonis-singh/re4)) | `external/CubeEvil4` (submodule) | its own `LICENSE` (CC0 1.0 for its build scripts, tools, configuration and documentation). The reconstructed game, SDK and middleware source stays the property of Capcom, Nintendo and CRI Middleware. |
| Boost.Context (fcontext assembly) | `port/third_party/boost_context` in the build tree | Boost Software License 1.0 (`LICENSE_1_0.txt` there) |
| zstd (decoder) | CubeEvil4, `port/android-decomp/app/src/main/cpp/third_party/zstd` | BSD 3-Clause (its `LICENSE` there) |
| SDL 3 (release-3.4.16) | fetched when PuebloPrep's window is built; linked into PuebloPrep | zlib licence (`licenses/SDL3-LICENSE.txt` in PuebloPrep's packages) |
| Dear ImGui (v1.92.9b) | fetched when PuebloPrep's window is built; linked into PuebloPrep | MIT (`licenses/imgui-LICENSE.txt` in PuebloPrep's packages) |
| Noto Serif JP, Noto Serif SC | glyphs drawn into `port/whole/3ds/ui_assets.h` and the .cia banner and icon | SIL Open Font License 1.1 ([licenses/OFL-1.1.txt](licenses/OFL-1.1.txt)) |
| CyrSlav-VGA8 (Debian console-setup) | the 8x8 font in `port/whole/3ds/hud_font.h` (eight glyphs - ► ◄ … □ ▓ Г П Ф - drawn for PuebloCTR, MIT) | public domain: console-setup builds CyrSlav-VGA8 from its `legacy8a`, `legacy8b`, `legacy8c`, `legacy8f` and `legacy8i` BDF fonts (`Fonts/Makefile`, `CyrSlav-legacy8-BDFS`), which its `copyright.fonts` lists as "automatically generated from console fonts. Because of that they are public domain" (and: "All console fonts are public domain by nature") |
| devkitPro (devkitARM, libctru, citro3d) | the build image; libctru and citro3d are linked into the game | zlib licence |
| makerom (Project_CTR), bannertool | the build image (making the .cia) | MIT |

PuebloCTR ships no game data. The game's files, textures and movies are made on the player's computer by PuebloPrep from
the player's own discs.
