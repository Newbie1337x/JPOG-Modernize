# Sky and weather plan

Plan for a new sky (real sun and moon, volumetric clouds), better water at night and a weather
system layered on top of the game's own. Nothing here is implemented yet.

## What we have today

### Our sky (`src/d3d8to11/sun.cpp`, `ProceduralSky`)

- Drawn in the composition pass on sky pixels (`depth >= 0.999999`), blended over the game's
  sky texture by `tod2.w` (night and dusk; by day the game's sky texture is kept).
- Sun: only a glow, `pow(sd, 900) * 4 + pow(sd, 12) * 0.18`. No disc.
- Moon: always opposite the sun (`moonDir = -tod2.xyz`), a small disc plus halo, no phases,
  never visible by day.
- Stars: a hash grid in sky angles, faded by `tod4.y`.
- The time of day is a fixed preview hour.

### Our water (`src/d3d8to11/shaders.cpp`)

- Foam is a constant colour, `lerp(surface, float3(0.80, 0.86, 0.78), foam)`, not lit, so it
  stays bright white at night.
- Moon glints come from `specParams.z` / `rimColor`; the moon disc itself is not reflected.

### The game's weather (`Data/Weather.ini`, `Data/Constant.ini`, `SimJP.exe`)

Five types, one fixed set of values each:

| Type | Wind (km/h) | Clouds | Sky texture |
|---|---|---|---|
| `normal` (0) | 0-9 | 60 sprites | `skynorm.dds` |
| `lightrain` (1) | 0-24 | 80 | `skyrainL.dds` |
| `heavyrain` (2) | 24-62 | 100 | `skyrain.dds` |
| `tornado` (3) | 62-96 | 100 | `skytorn.dds` |
| `heatwave` (4) | 0-8 | 0 | `skyheat.dds` |

Per type, `Weather.ini` also gives: dry/wet season lengths and probabilities, deep ocean colour,
water distortion, fog colour/near/far, a 4-colour sky gradient (front/back, top/bottom),
ambient and sun colours, cloud colour and min/max size (billboard sprites) and scattering
parameters (`pc_scatlight`).

`Constant.ini` lightning: a thunder every 10 s (`WeatherTimeBetweenThunder`), 20% of them with
lightning; lightning hits a dinosaur 20%, a structure 15%, a fence 15%, 200 damage each.

#### `AWeatherManager` (SimJP.exe)

`OnUpdate` (0x5f7b80): each weather lasts a season-dependent length; when it ends the next one
is drawn (`GetNewWeatherType` 0x5f83d0) and light, water, grass and wind fade linearly over the
fade length. The tornado queues its warning movie before it arrives.

Fields read from the decompiled code:

| Offset | Meaning |
|---|---|
| `+0x30` | time in the current weather |
| `+0x34` | length of the next weather |
| `+0x38` | fade length |
| `+0x3c` | length of the current weather |
| `+0x4c` | current type |
| `+0x50` | previous type (fading out) |
| `+0x54`, `+0x58` | next two types |
| `+0x64`, `+0x68` | fade states (2 = done) |
| `+0x200`, `+0x204` | grass modulate scale from / to (`UpdateFadeGrass` 0x5f9e00) |
| `+0x208` | wind direction (vector, rotated around Y during a fade) |
| `+0x214` | target wind direction |
| `+0x220`, `+0x224`, `+0x228` | wind angle from / to / current |
| `+0x22c` | wind strength (current) |
| `+0x230`, `+0x234` | wind strength from / to |
| `+0x238` | maximum wind strength (normaliser) |
| `+0x23c`, `+0x244` | wind scroll offsets (x, z) |
| `+0x2bc` | `ASky*` |
| `+0x2d4` | weather locked (keeps the current type) |
| `+0x2d8` | allowed-types mask from the menu |

`UpdateFadeWind` (0x5f97b0) writes the wind direction and the normalised strength to the
terrain's grass object (`m_pSimJPTerrain + 0x500`, fields `+0x44..+0x50`), and every few seconds
sends a random gust to the terrain (vtable `+0x54` / `+0x58` of `m_pSimJPTerrain + 0xd8`).

Other entry points: `GetCurrentWeather` 0x46a540, `GetWindDir`, `GetWindStrengthInKPH`,
`GetWindStrengthAtPosKPH`, `IsRaining`, `GetRainMag` 0x5fcd30, `GetWeatherInterp` 0x5fcd40,
`GetLightDirection` 0x5fcd10, `GetTornadoPos` 0x5f4b40, `GetWindMagAtPos` 0x5f4b90,
`OverrideNextWeather` 0x5fcd60, `SetWeather` 0x5f4910, `CreateLightningAtPos` 0x5f4c10,
`CreateThunderFlash` 0x5f4ce0, `RenderLightning` 0x5f4e90, `AParkManager::GetWeatherManager`
0x46df40.

## Approach

The game stays the director: it picks the weather, its length, the transitions and all gameplay
(lightning damage, visitor and dinosaur reactions). The plugin reads its state every frame and
passes it to the renderer; everything visual is ours, using `Weather.ini` only as a tint
reference.

## Stages

1. **Night water foam.** Light the foam with the scene light (sun/moon and ambient) instead of a
   constant colour, so it fades with the night.
2. **Real sun and moon.**
   - Sun disc with limb darkening and a correct glow; position from the hour.
   - Moon on its own orbit (not opposite the sun), lit by the sun direction so phases come out of
     the geometry, with the earthshine on the dark side; visible by day when it is up.
   - The moon disc and its glitter path reflected in the water.
   - Stars rotating with the hour, faded by the moon's brightness.
3. **Volumetric clouds.**
   - Ray-marched cloud layer (noise shapes, coverage, density) lit by the sun or the moon, with
     silver lining and the moonlit look of the reference image.
   - Only on sky pixels, at reduced resolution with temporal reprojection; nothing is computed
     where the terrain covers the sky.
   - Coverage, type and colour driven by the weather state.
4. **Our weather on top of the game's.**
   - Variants per period, from a seed per weather period: `normal` clear / few clouds /
     partly cloudy; `heavyrain` steady rain / thunderstorm; etc.
   - Smooth transitions following the game's fade (`+0x30 / +0x38`).
5. **Wind everywhere.** The game's direction and strength as the base, plus our own noise gusts
   (random in rain, furious in storms). One wind field moves grass, trees, clouds and the ocean
   swell and whitecaps.
6. **Rain and wet surfaces.** Rain streaks, splashes, wet darkening and puddle reflections,
   scaled by `GetRainMag`.
7. **Lightning.** Hook `CreateLightningAtPos` so our bolt, cloud flash and scene flash match the
   game's strikes and damage; extra distant flashes that are only visual.
8. **Clock.** Let time run (decision below) and make every stage follow it.

## Decisions

- Clock: time runs (day and night cycle), with a configurable day length; no calendar.
- Moon phases: an internal day counter advances the moon on its orbit, about one phase step per
  game day (a full cycle every ~8 game days by default, configurable), so nights vary without
  any date system.
- The moon is visible by day when it is above the horizon (follows from its own orbit).
- Shadows with a moving sun must never pop. The shadow cache already starts a new build as soon
  as one completes (every ShadowCacheFrames frames), always with the current sun, and the live map
  is drawn with the cache's direction: the shadows advance in steps of about 0.01 degree, which
  reads as continuous. Where the light hands over from the sun to the night light (different
  directions) the shadows dissolve and come back instead of jumping.

## Open decisions

- Default length of a game day.
- Other complaints about the water.
- New weather states the game lacks, or only variants of its five.

## Performance budget

Heaviest view today: ~6.85 ms (~146 fps). The clouds are the expensive part; target well under
1 ms with reduced resolution, temporal reprojection and skipping covered pixels.
