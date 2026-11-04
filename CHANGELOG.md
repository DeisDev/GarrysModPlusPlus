# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html). The first release will be 0.9.0-alpha.

## [Unreleased]

### Added

- The mod's version is shown at the bottom of the menu's page list and on the About page, and written at the top of the log.
- Debug views showing the image's brightness in nits and the pixels brighter than the display can show.

### Changed

- The game draws into a high dynamic range image, so nothing is clipped at white until the image is shown. The tone curve now covers everything on screen: particles, sprites, glass and the sky roll off with the rest of the image.
- The Brightness slider in the video options works again, in every display mode.
