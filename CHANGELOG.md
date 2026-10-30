# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Added

- A "Baked" indirect light option that reuses the map's own bounce light instead of tracing it, for faster frames; the Low preset uses it.
- Glass and other translucent surfaces are lit by real-time light instead of baked light.
- Ray-traced ambient occlusion, with an on/off switch and strength in the menu.
- Volumetric light from the sun and flashlights, with an on/off switch and strength in the menu.
- Debug views for reflections, motion vectors and ambient occlusion.
- Particles and effects that ask the engine for the light at a point now get real-time light.
- Ray tracing quality presets (Low, Medium, High, Ultra) and settings for shadow rays, indirect rays and bounces, ambient occlusion rays, light volume rays, volumetric light steps and foliage shadows.
- Opacity micromaps make ray tracing through grass, leaves and fences faster on GPUs that support them, with an option for faster, approximate alpha tests in indirect light.
- The ambient occlusion pass has its own GPU time in the menu and log.
- Shadows of swaying grass, leaves and trees (`$treesway`) now move with them.
- The menu and log show the GPU time of the 3D skybox and viewmodel, which the lighting total now includes.
- Ropes and cables, such as power lines, cast shadows.
- Held weapons, including your own, cast shadows and show in reflections, placed in the hand that holds them.
- Light switches, toggled lights and animated lights (flicker, pulse, scripted day and night) now work with real-time lighting.
- Water has ray-traced reflections over the game's refraction, a sun glint and a physical Fresnel term.
- Underwater, the surface shows the world above through Snell's window and mirrors the water below beyond it, and sunlight scatters in the water's colour.
- A `winmm.dll` loader in the game folder. With `-insecure -gmodpp_vanilla` it runs the game's own renderer through `gmodpp.dll`, as a reference for the coming native Direct3D 12 renderer.
- In the native renderer, GMod's Advanced video options have settings for anti-aliasing (TAA or DLSS), the DLSS mode, real-time lighting, ray tracing quality and indirect light. MSAA and Direct3D 9Ex are hidden there, and MSAA is turned off when the options are applied.
- In the native renderer, camera screens, mirrors and scopes that draw the world into a texture get real-time light ("Cameras and mirrors" in the menu; about 1 ms for a 512x512 view). The water's own views keep the game's light.

### Changed

- Shadows of many lights are less noisy: the strongest lights are always traced, and the rest are denoised.
- Reflections are steadier and sparkle less.
- Skin materials support more detail texture blend modes.
- Lights inside lamp models are blocked by the lamp the way the map's baked lighting was, so rooms are no longer brighter than intended.
- Grass and other props the map marks as casting no shadows now cast shadows from the sun.
- Values in `gmodpp_settings.ini` are limited to the ranges the menu offers.
- Grassy areas render faster: later indirect light bounces pass through grass, and alpha tests read less data.
- The DirectX 12 Agility SDK runtime is installed to `bin/win64/D3D12`; without it, opacity micromaps are unavailable.
- Spot light pools have soft edges like the map's baked lighting instead of a hard line.
- The ray-traced flashlight and other projected lights fall off with distance like real light beyond the range where they reach full strength.
- Maps with many lights render faster: lights are evaluated faster, the light volume is traced in parallel, and the viewmodel is lit faster.
- The flashlight shines from where you hold it instead of from up to 128 units behind you near walls, so its shadows no longer slide around as you walk.
- Objects right in front of you now block your flashlight.
- The flashlight's beam has a bright centre that fades out, as its texture intends, instead of a flat disc.
- The flashlight lights things close to you more brightly than things far away, like a real flashlight.
- The debug view that showed the game's flashlight pass now shows additive glows.
- Glows, sprites and other additive effects are added after anti-aliasing and DLSS, so they no longer jitter or smear.
- In third person, your own flashlight is treated like your first-person flashlight, so your body no longer blocks it.
- In the native renderer, glass and other translucent surfaces get sharp shadows from the sun and nearby lights.
- In the native renderer, eyes and teeth are lit by real-time light.
- In the native renderer, brushes and models with `$phong` get their highlights, shadowed by ray tracing: brushes from the sun as in GMod, models from the sun, the strongest lights and flashlights.
- Reflections of materials with `$envmaplightscale` fade where the surface is dark, as in the game.
- In the native renderer, bump-mapped models and skin materials with `$halflambert` (most characters) take light with the game's half-Lambert shading.

### Removed

- The Direct3D 9 build (`bin/win64/d3d9.dll` with D3D9On12). `-insecure` now starts the native Direct3D 12 renderer through the `winmm.dll` loader; deploying removes the old `d3d9.dll`.
- The "Game" flashlight option. It made props see-through and left them unlit by the flashlight.

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
- Fixed bright lines along the edges of walls, floors and ceilings, and along grooves in normal-mapped floors.
- Fixed window glass (Refract materials) blocking sunlight and daylight, which left rooms dark.
- Fixed dark spots in the player's shadow around the feet.
- Fixed the flashlight turning props, foliage and trees white.
- Fixed glass seen through small gaps (such as between stair steps) flickering.
- Fixed fog using the wrong colour, and the 3D skybox getting no fog on some maps.
- Fixed shadows of leaves and other alpha-tested surfaces looking like square blocks.
- Fixed objects right behind window glass showing a mirror image like a cubemap.
- Fixed the held weapon looking see-through in front of bright areas.
- Fixed maps without fog turning black a short distance away, as on gm_publicindoorswimmingpool.
- Fixed the flashlight's bounce light missing on the viewmodel where it comes from surfaces off screen.
- Fixed some props, such as plastic chairs, casting shadows as if they were turned the other way.
- Fixed rain, snow and other unlit particles disappearing with real-time lighting.
- Fixed the physics gun's glow on held objects flickering.
- Fixed thin bright lines along the edges of white window frames, as on gm_skywatching.
- Fixed baked indirect light counting dim light-emitting surfaces twice.
- Fixed dark patches in the "Baked" indirect light option where the map's baked shadows differ from the ray-traced ones.
- Fixed ray-traced lighting possibly reading the wrong textures or materials on some GPUs.
- Fixed NPC eyes showing no iris with real-time lighting in the native renderer.
- Fixed window glass turning white in the flashlight.
- Fixed the underwater fog being grey instead of the water's colour.
