# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Added

- A "Baked" indirect light option that reuses the map's own bounce light instead of tracing it, for faster frames; the Low preset uses it.
- Glass and other translucent surfaces are lit by real-time light instead of baked light.
- Ray-traced ambient occlusion for the "Baked" indirect light option, with an on/off switch and strength in the menu. Ray-traced indirect light already holds its own occlusion, so the setting is greyed out with it.
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
- In the native renderer, GMod's Advanced video options have settings for anti-aliasing (TAA or DLSS), the upscaling mode next to it and, in their own section, real-time lighting, ray tracing quality, indirect light, foliage shadows, reflections, ambient occlusion, volumetric light, translucent lighting and camera and mirror lighting. MSAA and Direct3D 9Ex are hidden there, MSAA is turned off when the options are applied, shadow detail is greyed out while real-time lighting is on, and the two DirectX level rows are replaced by one that names the renderer (DirectX 12).
- In the native renderer, a "Display" setting in the Advanced video options moves the game window to another monitor.
- In the native renderer, camera screens, mirrors and scopes that draw the world into a texture get real-time light ("Cameras and mirrors" in the menu, off by default; about 1 ms for a 512x512 view). The water's own views keep the game's light.
- The menu says why real-time lighting is paused: no ray tracing support, or multicore rendering on.
- In the native renderer, flowing water (`$flowmap`, also with a flowing base texture) and water with `$lightmapwaterfog` get ray-traced reflections like other water.
- In the native renderer, textured water (`$basetexture`) is lit in real time instead of by its baked light, with its ripples and ray-traced reflections.
- In the native renderer, models and world surfaces with a light warp texture take real-time light through it as in the game.
- Glowing surfaces such as screens and light panels light their surroundings, with a "Glowing surfaces" setting (Off, Bounce light, Full). Light textures the map already turned into lights keep the map's lights instead.
- In the native renderer, reflections show the sky as the game draws it, including painted skies, and the 3D skybox, instead of a flat colour.
- The project is licensed under the Mozilla Public License 2.0. `THIRD_PARTY_NOTICES.md` lists the parts under other licences.
- The settings menu's General tab credits NVIDIA DLSS and says where the licences are.
- A world-space radiance cache (NVIDIA SHaRC) gives bounced light many bounces at a lower cost, and lights what reflections and water show off screen. "Radiance cache" in the menu; it needs 64-bit atomics and 16-bit shader types.
- Shiny materials with a Phong exponent get glossy ray-traced reflections instead of mirrors, and DLSS Ray Reconstruction is told their roughness. "Glossy reflections" in the menu.
- Direct light from the map's lamps, glowing surfaces, dynamic lights and flashlights is chosen with NVIDIA RTXDI (ReSTIR DI with a ReGIR light grid): maps with hundreds of lights get every light's contribution with fewer rays. "Light sampling" in the menu; Auto reuses samples with TAA and uses importance sampling with DLSS Ray Reconstruction.
- Without DLSS Ray Reconstruction, indirect light and reflections are denoised by NVIDIA NRD (RELAX by default, REBLUR as an option), replacing the built-in filter. "Denoiser type" in the menu.
- Debug views for the radiance cache's voxels, RTXDI's light sample and reused samples per pixel, and NRD's validation overlay.

### Changed

- Reflected surfaces are lit with the sun, a lamp and the radiance cache where they are, instead of borrowing the reflecting surface's light or reusing last frame's image.
- The "Shadow rays" settings apply only to the Legacy light sampling mode.
- The credits name NVIDIA RTXDI, SHaRC and NRD, and their licences are in `gmodplusplus\licenses`.
- The mod keeps all its files in a `gmodplusplus` folder in the game folder: its DLLs, settings, logs, shader caches and developer files. Only the loader, `winmm.dll`, stays in the game folder itself.
- In the native renderer, indirect light from the sky takes the skybox's colours from each direction, at the map's sky light brightness.
- The native renderer always turns MSAA off, so real-time lighting no longer needs it turned off by hand.
- Shadows of many lights are less noisy: the strongest lights are always traced, and the rest are denoised.
- Reflections are steadier and sparkle less.
- Skin materials support more detail texture blend modes.
- Light through glass and other translucent surfaces takes their colour: stained glass throws coloured light, and Refract glass windows tint the light by their own tint instead of letting it all through.
- Map lights are no longer limited to the areas the map's visibility data says they can see, so light that comes through gaps it misses still arrives. Each spot evaluates at most 64 lights and samples the rest.
- Map lights no longer stop at the maximum distance set in the map, which the map's own baked lighting ignored too, and faint lights no longer switch on and off as the exposure changes.
- Map lights fall off with distance like real light, from a bulb-sized source with soft shadow edges. Each light keeps the brightness its mapper set at 100 units (or at its 50% distance), so most lights look the same there; lights set to fade linearly or not at all are brighter up close and dimmer far away.
- Character skin scatters light under its surface, softening and reddening shadow edges up close.
- Leaves, grass and other props the map lights without their normals are lit from both sides: 30% of the light passes through them and scatters forward, so they glow when the sun is behind them.
- Lamp models block light with their real shape: light leaves open lamps through their openings, and lights sealed inside a lamp shine through it as through frosted glass.
- Grass and other props the map marks as casting no shadows now block all light: the sun, lamps and bounced light. The "Foliage shadows" setting can limit this for speed in grassy maps.
- Values in `gmodpp_settings.ini` are limited to the ranges the menu offers.
- Grassy areas render faster: alpha tests read less data.
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
- Self-illuminated surfaces such as screens and light panels glow in reflections even when they are off screen.
- Ambient occlusion is steadier while the camera moves.
- In the native renderer, bump-mapped models and skin materials with `$halflambert` (most characters) take light with the game's half-Lambert shading.
- In the native renderer, characters with `$rimlight` (many Half-Life 2 models and the first-person hands) get their rim highlights from real-time light.
- In the native renderer, glass and other translucent surfaces no longer hitch the first time they appear: their shaders are prepared with the rest after a game update.

- In the native renderer, "Wait for vertical sync" works in a window and borderless too, not only in full screen.
- Volumetric haze is much thinner and scatters more towards the light, so maps without fog no longer look hazy; it shows mainly as sun shafts and light beams.
- With volumetric light on, a map's fog is lit by the real lights: it keeps the map's fog colour in open sunlight, darkens in shadow and indoors, glows around lamps, and shows sun shafts and flashlight beams. Translucent surfaces and the 3D skybox get the lit fog too. Fog under water is unchanged.

### Removed

- The Direct3D 9 build (`bin/win64/d3d9.dll` with D3D9On12). `-insecure` now starts the native Direct3D 12 renderer through the `winmm.dll` loader; deploying removes the old `d3d9.dll`.
- The "Game" flashlight option. It made props see-through and left them unlit by the flashlight.

### Fixed

- Bounced light now carries lamp light past the first bounce; rooms lit only by lamps were darker than they should be.
- Light on glass and other translucent surfaces near the player updates every frame instead of every fourth.
- Glowing surfaces light their surroundings with far less speckle on maps with many of them.
- Opacity micromaps now speed up shadow, indirect and reflection rays as intended, not only light volume and volumetric rays.
- Fixed mirrors washing out with the flashlight on, and far walls in mirrors, camera screens and glass being lit with the sky's light or with light from behind them. Mirrors now get real-time light.
- Fixed light from lamps fading in and out as the camera moves closer or further away.
- Fixed holes in the first-person hands and other rim-lit skin where the surface faces the camera.
- Fixed UnlitTwoTexture and MonitorScreen materials being lit in the native renderer.
- Fixed jagged shadow shards on characters where their surface turns away from a light.
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
- Fixed ray-traced shadows ignoring the transparent parts of textures stored in less common formats.
- Fixed props and other models with a non-default skin being lit as their first skin, which could make static props glow at full brightness.
- Fixed NPC eyes showing no iris with real-time lighting in the native renderer.
- Fixed window glass turning white in the flashlight.
- Fixed the underwater fog being grey instead of the water's colour.
