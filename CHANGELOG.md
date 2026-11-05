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
- The Performance page and the overlay show the camera effects' GPU time per pass.
- The held weapon and hands shadow each other, from the sun, lamps, the flashlight and muzzle flashes (Lighting page, on by default). Each weapon's vertex shaders compile once more the first time it is held.
- Specular anti-aliasing: bumpy and shiny surfaces far away no longer sparkle in motion (Lighting page, on by default).
- Ground mist (Lighting page, off by default): a layer of mist over the ground that drifts with the wind, lit by the sun, sky and lamps, with light shafts through it.
- Distant air (aerial perspective): far-off land in the 3D skybox turns hazy and blue by day and red at sunset, and cloud shadows show as dark rays across the sky.
- Ice halos, sun dogs, the 46° halo, the circumzenithal arc and coloured coronas around the sun and Moon, from the clouds (Sky page).
- Smoke and other particles that blend cast soft shadows from the sun on the ground, in the haze and mist, and on the particles born under them (Lighting page, on by default).
- Local exposure on the Image page (off by default): brightens dark areas and darkens bright ones while keeping their detail.

### Changed

- The game draws into a high dynamic range image, so nothing is clipped at white until the image is shown. The tone curve now covers the whole 3D view: particles, sprites, glass and the sky roll off with the rest of the image. The HUD and menus keep their colours.
- The Brightness slider in the video options works again, in every display mode.
- Maps' colour correction is applied after the tone curve, so it no longer clips bright light.
- The sun and Moon are drawn at their real brightness, so on an HDR display they reach its peak instead of stopping at white.
- The Filmic tone curve is Gran Turismo's in SDR as in HDR and keeps hues; it replaces AgX.
- The menu blends over the game in linear light in SDR as in HDR.
- The lens flare is now the reflections inside a real double Gauss lens: coloured aperture shapes along the line through the sun that grow, shrink and brighten with the f-number, with coating colours, colour fringes and a diffraction starburst. New settings for aperture blades, colour fringes and starburst size.
- Cloud shadows depend on height: places above or inside the clouds are shadowed only by the cloud above them.
- A new bloom replaces the game's: bright light such as the sun now glows by how bright it really is, and the bloom spreads light without adding any. It still follows the game's bloom scale and has strength and spread settings on the Camera page.

### Fixed

- Clouds no longer repeat in a pattern toward the horizon.
- Fixed the game crashing when ARC9 weapons draw their icons, such as when opening the weapon selector.
- Images that addons read back from render targets keep their transparency, so ARC9's weapon icons no longer have a black background.
- Less noise in the indirect light on the held weapon and hands.
- Heat distortion from explosions no longer shows a box with part of the map in it.
- ARC9's customization menu shows the game behind it again instead of a white screen.
- Fixed dark streaks along the edges of hands and other models.
- Distant power lines and ropes against the sky no longer break into dashes with DLSS Ray Reconstruction or FSR.
- Lamps in fog and water are much less noisy in the distance.
- The HUD no longer smears in generated frames with DLSS Frame Generation.
- Light shafts in haze no longer stop at the edges of things against the sky with DLSS Ray Reconstruction or FSR.
- Under water with DLSS Ray Reconstruction or FSR, the far water no longer shows the unlit view above the surface.
