# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Added

- Glass and other translucent surfaces are lit by real-time light instead of baked light.
- Ray-traced ambient occlusion, with an on/off switch and strength in the menu.
- Volumetric light from the sun and flashlights, with an on/off switch and strength in the menu.
- Debug views for reflections, motion vectors and ambient occlusion.
- Particles and effects that ask the engine for the light at a point now get real-time light.

### Changed

- Shadows of many lights are less noisy: the strongest lights are always traced, and the rest are denoised.
- Reflections are steadier and sparkle less.
- Skin materials support more detail texture blend modes.
- Lights inside lamp models are blocked by the lamp the way the map's baked lighting was, so rooms are no longer brighter than intended.
- Grass and other props the map marks as casting no shadows now cast shadows from the sun.
- Values in `gmodpp_settings.ini` are limited to the ranges the menu offers.

### Fixed

- Fixed bright green or white specks in the lighting, most visible with DLSS.
- Fixed signs, screens and other unlit materials being lit.
- Fixed smearing on moving objects with DLSS.
- Fixed foliage, fences and glass casting no shadows.
- Fixed black shadow streaks on displacement terrain such as the gm_construct hill.
- Fixed bullet holes and other darkening decals disappearing with real-time lighting.
- Fixed maps saved with compressed data getting no real-time lighting.
- Fixed walls and floors whose material tints their lighting (`$color2`) looking too bright.
- Fixed grass and other foliage turning black where it faces away from the light.
- Fixed ghost copies of nearby objects appearing in the sky with DLSS Ray Reconstruction.
