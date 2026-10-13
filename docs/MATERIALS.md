# Texture-scoped material pilot

Only a few normal-map identities tag a texture when the existing plugin calls
`D3D8to11_SetNormalMap`: `dinos_hellcreek_b__tex5_n.dds` / `tex4_n.dds` (profile 1,
Triceratops adult and juvenile skins) and `Structures_Hi__Hamburger_n.dds` (profile 2).
Matching is case-insensitive basename. Dinosaur libraries name materials, not
textures (see docs/NORMAL_MAPS.md): `dinos_hellcreek_b__Tricera` is texture 2, a
Pachycephalosaurus skin, while the Triceratops is drawn with textures 5 and 4.
The second is the burger-shop ornament, not the entire building/shared roof atlas.
Multiple instances sharing either texture receive the same profile. No game
texture, normal DDS, plugin or save is modified; no material masks are generated.

Main-world skinned/rigid draws only, no water, grass, terrain, HUD or shadow pass.
Routing now accepts the tagged texture at t1 as well as t0: the captured skin
shader blends both with c1.w. Stage 1 is considered only when the PS actually
samples it, avoiding stale bindings; its normal map uses i.t1.xy. Sampling
metadata is decoded at shader creation, not per draw. GUI shows current-frame
pilot draw counts and optional flat magenta (skin) / cyan (ornament) diagnostic
colors, disabled by default and not persisted. A zero count means no pilot draw
was submitted in the main view, regardless of the enabled checkbox.
Sun control enables existing lighting bypass on selected draws only: translated
PS removes original vertex diffuse/specular and defers recognized fog. VS emits
skinned normals using the existing inverse-transpose path. Other draws retain
their original lighting/normal-map relief. Texture-baked lighting remains in
base color; this is not full albedo recovery or full-game PBR conversion.

Material PS variants use a 32-byte dynamic b3 CB (separate from VS grass b3),
reused for both profiles. GGX distribution, Schlick Fresnel/Smith visibility,
dielectric F0=.04, gamma-2.2 color conversion, normal maps through derivative
tangent frames, and diffuse/specular sunlight are evaluated per pixel. Ambient
hemisphere + reflected sky gradient is analytic approximation, not captured
cubemap IBL, scene reflection or GI. Existing scene shadows/AO/post remain.
Alpha test, texture UVs and alpha output are preserved. Material variants skip
legacy normal relief and ApplySun, avoiding two direct-light passes in the PS.

`[Materials] Enabled=1`, SkinRoughness=.58, BuildingRoughness=.78, Specular=.85,
Environment=.25, NormalStrength=.65. Uniform roughness per selected texture:
skin vs horn/eyes and wood vs metal cannot yet differ within an atlas. There
are no roughness/metalness/AO material maps in this initial pilot. Controls are
in Insert with immediate on/off comparison; no global ReplaceOriginal change.

Validation: 76 captured VS/PS/material/shadow/water variants compile. Hidden
MSAA8 fixture uses both actual normal DDS files and tagged reflection textures:
skin on f6fbfe vs off f5f5f5; ornament on f0f5f8 vs off ffffff. HUD remains exactly
green and four scene resets remain stable. Log confirms profile 1 dispatch.
These flat-quad fixtures establish routing/output, NOT real dinosaur/building
visual quality. Native game validation still requires the user; game not launched.

Resource/create/map failure disables the pilot with a log reason; the failing
draw can be skipped before subsequent draws resume the original route. No
per-frame texture allocations or readback is added. First material shader use
can compile a new variant. Existing normal files generated from luminance are
approximations; authored maps/masks remain the next quality improvement.
