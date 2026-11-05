# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.9.0-alpha] - 2026-11-05

First public release. This is an early alpha: expect bugs, crashes and addons that don't work with it.

### Added

- A Direct3D 12 renderer that replaces the game's Direct3D 9 one and runs the game's own shaders. It loads only when the game is started with `-insecure`.
- Ray-traced lighting in place of the maps' baked lighting: sun and lamp shadows, bounce light and sky light.
- Ray-traced reflections on shiny surfaces, water and glass, in place of the maps' cubemaps.
- Ray-traced flashlights and projected lights.
- Glowing surfaces and models, muzzle flashes, explosions, fire and other effects light their surroundings.
- Coloured light through stained and tinted glass, light through leaves, and soft light on skin.
- Water with traced reflections and refraction, caustics and light shafts under the surface.
- Volumetric light: lit map fog, haze with light shafts, optional ground mist, and shadows from smoke.
- A physical sky with a sun disc, sunsets and dark nights, stars, the Milky Way, a Moon with phases, and volumetric clouds with changing weather and cloud shadows.
- The sun, sky and shadows follow the sandbox Sun Editor, and the menu can set the sun, sky light and fog by hand.
- NVIDIA DLSS Ray Reconstruction, AMD FSR upscaling and FSR Ray Regeneration, alongside the mod's own anti-aliasing and denoisers.
- DLSS and FSR Frame Generation, NVIDIA Reflex and AMD Anti-Lag 2, off by default.
- HDR10 output on HDR displays, and 10-bit output on SDR displays that take it.
- A tone curve that keeps bright light from clipping, colour grading and local exposure.
- A bloom that spreads bright light without adding any, in place of the game's.
- Camera effects, off by default: depth of field, motion blur, lens flare, vignette, sharpening, film grain, chromatic aberration and lens distortion.
- Quality presets from Low to Ultra; Low bakes bounce light when a map loads, for slower GPUs.
- A settings menu (Insert) with per-map settings, your own presets, a performance overlay and a log viewer.
- Menu sliders take typed values (click the number) and step with the arrow keys, ten steps with Shift.
- A check at launch that warns about GPUs that can't run the mod and offers to play without it.
- Addons can read the `gmodpp_lights` console variable to turn off their own dynamic lights while the mod lights effects itself.

### Fixed

- Fixed depth of field turning out-of-focus areas into blocky, low-resolution squares.
- Fixed wide depth of field blur breaking up at low quality settings.
- Fixed motion blur streaks ending in a hard edge instead of fading out.
- Fixed sharpening skipping everything brighter than paper white in HDR.
- Fixed Escape closing the menu while typing in a text field.

[Unreleased]: https://github.com/DeisDev/GarrysModPlusPlus/compare/v0.9.0-alpha...HEAD
[0.9.0-alpha]: https://github.com/DeisDev/GarrysModPlusPlus/releases/tag/v0.9.0-alpha
