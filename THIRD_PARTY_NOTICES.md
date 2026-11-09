# Third-party notices

## Xenia

`tools/data/xam_table.inc` and `tools/data/xboxkrnl_table.inc` are export tables from the
[Xenia](https://github.com/xenia-project/xenia) Xbox 360 emulator, used under its BSD license:

```
Copyright (c) 2015, Ben Vanik.
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

* Redistributions of source code must retain the above copyright notice, this
  list of conditions and the following disclaimer.

* Redistributions in binary form must reproduce the above copyright notice,
  this list of conditions and the following disclaimer in the documentation
  and/or other materials provided with the distribution.

* Neither the name of the project nor the names of its
  contributors may be used to endorse or promote products derived from
  this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

## NFL Head Coach 09 for PC

The Vulkan renderer and frame pacing in `src/vk` (everything there except the adapter files `vk_adapter.cpp`,
`vk_select.h`, `vk_stubs.cpp`, `runtime.h` and `kernel.h`, and this project's additions `widescreen.h`, `gpu_vk/vk_scene_effects.*`,
`gpu_vk/vk_upscale.*` and `gpu_vk/vk_motion.*`) come from NFL Head Coach 09 for PC 1.1.3, used under its MIT license (`src/vk/LICENSE`):

```
Copyright (c) 2026 ibukun5588 and the NFL Head Coach 09 for PC contributors
```

## AMD FidelityFX Super Resolution 1

The upscaling (EASU) and sharpening (RCAS) shaders in `src/vk/gpu_vk/vk_upscale.cpp` are ported from
[FidelityFX FSR 1](https://github.com/GPUOpen-Effects/FidelityFX-FSR) (`ffx_fsr1.h`, `ffx_a.h`), used under its
MIT license:

```
Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```

## nflverse player statistics

HC27's players' career and season statistics (the `player_stats` patches in `hc27/disc`, written by
`tools/roster_import/player_stats.py`) are derived from the regular-season player statistics of
[nflverse-data](https://github.com/nflverse/nflverse-data) (release `stats_player`), licensed
[CC-BY 4.0](https://creativecommons.org/licenses/by/4.0/). They were reduced to the fields the game
keeps and arranged in its career database's tables.

## Fetched separately (not included in this repository)

- With `HDTextures=on`, `HC09HDPack.exe` downloads
  [Real-ESRGAN ncnn Vulkan](https://github.com/xinntao/Real-ESRGAN-ncnn-vulkan) (MIT) with the
  [Real-ESRGAN](https://github.com/xinntao/Real-ESRGAN) models (BSD 3-clause) into `thirdparty/realesrgan`
  on the player's PC, checked against a pinned SHA-256.

- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) — MIT; modified by `patches/xenonrecomp.patch`.
- [FFmpeg](https://ffmpeg.org) — LGPL 2.1+ shared build, dynamically linked.
- For the Vulkan renderer, downloaded into `thirdparty/vulkan` when the project is configured:
  [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) (Apache 2.0 / MIT),
  [volk](https://github.com/zeux/volk) (MIT; loads the Vulkan driver at run time) and the
  [glslang](https://github.com/KhronosGroup/glslang) release package with SPIRV-Tools (BSD 3-clause / Apache 2.0;
  statically linked).
