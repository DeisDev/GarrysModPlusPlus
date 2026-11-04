# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html). The first release will be 0.9.0-alpha.

## [Unreleased]

### Added

- The mod's version is shown at the bottom of the menu's page list and on the About page, and written at the top of the log.
- HDR10 output on HDR displays, with paper white, peak brightness, black level, saturation and HUD and menu brightness on the Display page. Paper white and peak follow Windows and the display unless set by hand.
- DLSS and FSR Frame Generation work with HDR output.
- 10-bit SDR output where the display takes it.
- In HDR, the HUD and menu show at paper white or a fixed brightness, and a brightness boost (off by default) can lift the game toward middle grey by up to one stop.
- Debug views showing the image's brightness in nits and the pixels brighter than the display can show.

### Changed

- The game draws into a high dynamic range image, so nothing is clipped at white until the image is shown. The tone curve now covers the whole 3D view: particles, sprites, glass and the sky roll off with the rest of the image. The HUD and menus keep their colours.
- The Brightness slider in the video options works again, in every display mode.
- Maps' colour correction is applied after the tone curve, so it no longer clips bright light.
- The sun and Moon are drawn at their real brightness, so on an HDR display they reach its peak instead of stopping at white.
- The Filmic tone curve is Gran Turismo's in SDR as in HDR and keeps hues; it replaces AgX.
- The menu blends over the game in linear light in SDR as in HDR.

### Fixed

- Clouds no longer repeat in a pattern toward the horizon.
