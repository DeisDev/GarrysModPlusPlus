# GarrysMod++

A graphics mod for the 64-bit branch of Garry's Mod. It replaces the game's Direct3D 9 renderer with a native Direct3D 12 one that runs the game's own shaders, and replaces the baked lighting with ray-traced lighting: shadows, bounce light, reflections, water, glass, volumetric light, a physical sky with clouds and a night sky. It also adds DLSS and FSR upscaling, frame generation, camera effects and an in-game settings menu.

## Requirements

- Garry's Mod, 64-bit branch (`x86-64` beta), on Windows 10 or 11.
- A GPU with Direct3D 12 feature level 12_0, Shader Model 6.6 and resource binding tier 3. Without DirectX Raytracing 1.1 the mod runs with the map's baked lighting.
- For ray-traced lighting at playable frame rates: an NVIDIA GeForce RTX 3060 / AMD Radeon RX 6700 XT or better, with DLSS or FSR upscaling. The mod was developed on an RTX 4070 Ti at 2560 × 1440.
- A current graphics driver.

## Install

1. Download `GarrysModPP.zip` from the releases page and extract it.
2. Copy `winmm.dll` and the `gmodplusplus` folder into the game folder (the one with `gmod_win64.exe`), for example `C:\Program Files (x86)\Steam\steamapps\common\GarrysMod`.
3. In Steam, open the game's Properties and add `-insecure` to the launch options. The mod only loads with it (it keeps VAC out of the picture), so you can't join VAC-secured servers while it is on. Remove it to play without the mod.
4. Start the game. Press **Insert** to open the settings menu; the Menu page lets you change the key.

The first launch compiles shaders for a while. The game continues on its own when they are ready, and they are kept for later launches.

To uninstall, delete `winmm.dll` and the `gmodplusplus` folder from the game folder and remove `-insecure`.

## Settings

Everything lives in the in-game menu (Insert): Display, Graphics, Image (anti-aliasing, upscaling, frame generation), Lighting, Sky, Camera, Performance, Profiles (per-map settings, presets, import and export), Logs, Menu and About. Settings are saved in `gmodplusplus\settings.ini`; logs are in `gmodplusplus\logs`.

## Building from source

Toolchain: Visual Studio 2026 Build Tools (MSVC 14.51 or newer) with the Windows 11 SDK (10.0.26100), CMake 3.28 or newer and Ninja. CMake downloads the other dependencies, pinned by hash: the DirectX Shader Compiler, the DirectX 12 Agility SDK, the NVIDIA DLSS SDK, NVIDIA Streamline, NVIDIA SHaRC, NVIDIA RTXDI, NVIDIA NRD, the AMD FidelityFX and FSR SDKs, the Source SDK 2013 shader headers, MinHook, Dear ImGui and the LZMA SDK. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for versions and licences.

From a shell with `vcvars64.bat` loaded:

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo "-DGMOD_DIR=C:/path/to/GarrysMod"
cmake --build build --target deploy
```

`deploy` copies the mod into the game folder given by `GMOD_DIR` (close the game first). `cmake --build build --target release` packages it as `build/release/GarrysModPP.zip` instead.

### Contributing

Open an issue or a pull request on GitHub: <https://github.com/DeisDev/GarrysModPlusPlus>. Keep changes small and focused, note user-visible changes in `CHANGELOG.md`, and add any new third-party component to `THIRD_PARTY_NOTICES.md`. Facts about how the game behaves must come from reverse engineering or documentation, not guesses.

## Licence

GarrysMod++ is licensed under the [Mozilla Public License 2.0](LICENSE). The shaders under `shaders/twins/` are modified Source SDK 2013 shaders under Valve's Source 1 SDK License. The NVIDIA DLSS and Frame Generation libraries ship only inside the release zip. Made by DeisDev ("cat sniffer").
