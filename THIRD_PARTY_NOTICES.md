# Licences

Except where noted below, the files in this repository are subject to the terms of the Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed with this file, You can obtain one at <https://mozilla.org/MPL/2.0/>. The full text is in [LICENSE](LICENSE).

## In this repository

| Files | Licence | Notes |
| --- | --- | --- |
| `shaders/twins/` | Source 1 SDK License (Valve) | Modified Source SDK 2013 shader source, commit `7309a5f`. May only be distributed free of charge, with [shaders/twins/LICENSE](shaders/twins/LICENSE) and [shaders/twins/thirdpartylegalnotices.txt](shaders/twins/thirdpartylegalnotices.txt). |
| `SpaceFillingCurveIndex` in `src/opacity_micromaps.cpp` | CC BY 4.0 (Khronos Group) | Adapted from the reference code in the Vulkan specification's `VK_EXT_opacity_micromap` appendix. |

## Downloaded at build time

These are not in the repository. CMake downloads them, pinned by version and hash.

| Component | Version | Licence | Used for |
| --- | --- | --- | --- |
| MinHook (with Hacker Disassembler Engine) | 1.3.4 | BSD 2-Clause | Built into `winmm.dll` and `gmodpp.dll` |
| Dear ImGui | 1.91.9b | MIT | Built into `gmodpp.dll` |
| LZMA SDK decoder | 26.04 | Public domain | Built into `gmodpp.dll` |
| Source SDK 2013 shader headers | `7309a5f` | Source 1 SDK License (Valve) | Copied with the twins and compiled at runtime |
| DirectX Shader Compiler | 1.9.2602 | LLVM Release License, MIT, Microsoft software licence terms | `dxcompiler.dll` |
| DirectX 12 Agility SDK | 1.619.6 | Microsoft software licence terms | `D3D12Core.dll` and `d3d12SDKLayers.dll` |
| NVIDIA DLSS SDK | 310.9.16 | NVIDIA RTX SDKs License | Linked into `gmodpp.dll`, and `nvngx_dlssd.dll`. Not to be distributed on its own. |
| NVIDIA SHaRC | 1.8.3 (commit `4e21b585`) | NVIDIA RTX SDKs License | Compiled into `gmodpp.dll`'s lighting shaders. Not distributed as source. |
| NVIDIA RTXDI (RTXDI-Library) | 3.1.0 (commit `f12037fa`) | NVIDIA RTX SDKs License | Compiled into `gmodpp.dll` and its lighting shaders. Not distributed as source. |
| NVIDIA NRD | 4.18.0 | NVIDIA RTX SDKs License | Static library in `gmodpp.dll`. Not distributed as source. |
| MathLib | 11 | MIT | Built into NRD |
| ShaderMake | commit `3f01623a` | MIT | Its `ShaderMakeBlob` is built into NRD; the tool compiles NRD's shaders at build time and is not shipped |
| AMD FidelityFX SDK (Depth of Field, CAS, Lens) | 1.1.4 (commit `c6efa6bf`) | MIT | Compiled into `gmodpp.dll`'s post effect shaders |

This software contains source code provided by NVIDIA Corporation.

NVIDIA, NVIDIA RTX, DLSS and NVIDIA DLSS are trademarks of NVIDIA Corporation. GarrysMod++ uses NVIDIA DLSS Ray Reconstruction, NVIDIA SHaRC, NVIDIA RTXDI and NVIDIA NRD.

AMD and FidelityFX are trademarks of Advanced Micro Devices, Inc.

## Releases

The `release` build target packages the mod as `build/release/GarrysModPP.zip`. Everything but the loader (`winmm.dll`) goes in the game's `gmodplusplus` folder. Each component's licence text is in `gmodplusplus/licenses`, and Valve's licence files are next to the twins.

Source code: <https://github.com/DeisDev/GarrysMod-->.
