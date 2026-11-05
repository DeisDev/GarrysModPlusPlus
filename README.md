# GarrysMod++

A graphics mod for the 64-bit version of Garry's Mod. It replaces the Direct3D 9 renderer with a native Direct3D 12 one and the baked lighting with ray tracing: shadows, bounce light, reflections, water, glass, volumetric light, a physical sky with clouds and a night sky. It adds DLSS and FSR upscaling, frame generation, camera effects and a settings menu.

> **This is a proof of concept.** It is one person's experiment in how far Garry's Mod can be pushed, not a finished product. Expect bugs, crashes, broken addons, visual glitches and performance problems. Use it at your own risk and keep a backup of your settings.

GarrysMod++ is an unofficial fan project. It is not affiliated with or endorsed by Facepunch Studios, Valve, NVIDIA or AMD.

## Requirements

- Garry's Mod, 64-bit (the default public version, `gmod_win64.exe`), Windows 10 or 11.
- A GPU with Direct3D 12 feature level 12_0, Shader Model 6.6 and resource binding tier 3. Without DirectX Raytracing 1.1 the mod keeps the map's baked lighting.
- For playable ray tracing: RTX 3060 / RX 6700 XT or better, with upscaling. Developed on an RTX 4070 Ti at 2560 × 1440.
- A current graphics driver.

## Install

1. Download `GarrysModPP.zip` from the releases page and extract it.
2. Copy `winmm.dll` and the `gmodplusplus` folder into the game folder (the one with `gmod_win64.exe`).
3. Add `-insecure` to the game's launch options in Steam. The mod only loads with it, so VAC-secured servers are off limits while it is on.
4. Start the game. Press **Insert** for the settings menu.

The first launch compiles shaders for a while; later launches reuse them.

To uninstall, delete `winmm.dll` and the `gmodplusplus` folder and remove `-insecure`.

## Settings

Everything is in the menu (Insert). Settings are saved in `gmodplusplus\settings.ini`, logs in `gmodplusplus\logs`.

## Addon compatibility

The mod replaces the game's lighting, sky, fog and post-processing. Addons that do the same job will conflict: they may do nothing, light things twice or draw over the image. Turn them off while the mod is on. Likely conflicts:

- Dynamic light addons, such as [Better Lights](https://steamcommunity.com/sharedfiles/filedetails/?id=3597784225). The mod lights muzzle flashes, glowing models and effects itself; addons can check `GetConVar("gmodpp_lights"):GetBool()` and turn their own lights off while it is true.
- Screen-space shader and post-processing addons, such as gShader (ambient occlusion, bloom, reflections, sharpening, colour grading).
- Shadow addons built on projected textures, and lightmap or relighting addons.
- Sky and weather addons that paint the sky or move the sun, such as StormFox 2. The sun follows them; the painted sky, fog and night darkening do not.
- Binary modules (`gmcl_*.dll`) that use Direct3D 9 directly.

Addons that only add content (maps, models, weapons, NPCs, gamemodes) mostly work.

## Building from source

Visual Studio 2026 Build Tools (MSVC 14.51+), Windows 11 SDK 10.0.26100, CMake 3.30+ and Ninja. CMake downloads the other dependencies pinned by hash; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

From a shell with `vcvars64.bat` loaded:

```text
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo "-DGMOD_DIR=C:/path/to/GarrysMod"
cmake --build build --target deploy
```

`deploy` copies the mod into the game folder (close the game first). `release` packages it as `build/release/GarrysModPP.zip`.

### Contributing

Issues and pull requests: <https://github.com/DeisDev/GarrysModPlusPlus>. Keep changes small, note user-visible changes in `CHANGELOG.md`, list new third-party components in `THIRD_PARTY_NOTICES.md`. Facts about the game come from reverse engineering or documentation, never guesses.

## Licence

[Mozilla Public License 2.0](LICENSE). The shaders under `shaders/twins/` are modified Source SDK 2013 shaders under Valve's Source 1 SDK License. The NVIDIA DLSS and Frame Generation libraries ship only inside the release zip. Made by DeisDev ("cat sniffer").
