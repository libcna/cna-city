# What CNA City found in CNA

CNA City exists to push the CNA runtime until something bends and then say precisely what bent.
This file is that list.

Every item below was **checked against CNA's own source before it was written down**, and the
check is quoted. Three things that looked like engine defects turned out to be this program's
mistakes; they are in §C rather than deleted, because a finding list that only contains hits is not
a measurement, and because somebody will otherwise chase them into the engine.

Measured against `cnanext` and `sharp-runtimenext` as of 2026-09-03, `CNA_GRAPHICS_RENDERER=OPENGLES3`
(EasyGL) on GL ES 3.2, Mesa 25.0.7, AMD Radeon 780M.

---

## A. Capability gaps

### A1. A surface can have many lights, or a texture set. Never both.

`ClusteredForwardEffect` is the engine layer's own PBR effect and is what carries the light count
(`kMaxLightsPerFragment = 128`). `PbrEffect` is what carries the material: albedo, normal,
metallic-roughness, emissive, occlusion, plus the KHR extensions. They are different effects, and a
draw call picks one of them.

`NEXT_modern.md` already records why — `PbrEffect` owns no shader source, so a light loop there
would be a change to EasyGL's built-in effect family compiled into every game whether `CNA_CNAEXT`
is on or off (`MOD-2045`, `MOD-2062`). The boundary is understood. It is listed here anyway because
**it is the first wall a real game hits**, and because a city is the case that makes it concrete:

> CNA City has about 14 000 street lamps, several thousand headlights, and tens of thousands of lit
> windows. Every one of them is emissive geometry plus bloom. Not one of them casts light on the
> road. A city at night is *made of* the pools of light under its lamps, and this is the one thing
> the demo cannot show.

**Suggested:** give `ClusteredForwardEffect` the `PbrEffect` texture set. The layer owns that
shader, so it is a change inside CNAEXT rather than in the renderer's built-in effect family — and
`PbrMaterialExtensions` already establishes the pattern of carrying material data beside a material
for an effect that can consume it.

### A2. There is no attribute slot left for a per-instance colour.

`InstancedRendererEXT::getInstanceDeclaration()` puts the per-instance world matrix in four
`Vector4` elements at `TextureCoordinate` usage indices 1–4, bound to locations 12–15. The stock
lit shaders use 0–11 for the mesh. Sixteen is XNA's own ceiling and GL ES 3's guaranteed minimum, so
there is no seventeenth slot. `setTintsEnabled` exists and its own documentation says why it cannot
help: it needs a `ShaderEffect` whose mesh declaration is small enough to leave room, which means
giving up the stock lighting.

**What it costs here:** a hundred thousand people and several thousand vehicles are bucketed into
eight clothing colours and eight paints and drawn once per bucket — about fifty extra draw calls,
paid purely to avoid a uniformly grey crowd.

**Suggested:** a second, compact instance declaration — a 3×4 affine transform in three `Vector4`s
plus one packed `Color` — occupying the same four slots. A per-instance transform has no use for
the fourth row, which is always (0, 0, 0, 1). It costs nothing at the vertex stage and removes the
problem entirely.

### A3. `Texture2D` allocates a mip chain but never fills one, and the parameter says "generate".

`Texture2D(device, width, height, mipMap, format)` sets
`levelCount_ = mipMap ? CalculateMipLevels(w, h) : 1` (`modules/graphics/src/Xna/Texture2D.cpp`),
and `SetData` writes exactly the level it is given. There is no `GenerateMipMaps` anywhere in the
class. That is faithful to XNA, where mip levels arrive from the content pipeline — but a
procedurally generated game has no content pipeline, and a texture whose lower levels were never
written shows it the instant the camera pulls back. A city is almost entirely minified.

The header currently reads:

> `@param mipMap  True to generate a full mipmap chain.`

It allocates one. CNA City builds every chain by hand, box-filtered **in linear light** — about
forty lines that every CNAEXT consumer generating its own textures will now write again, and get
wrong in the same way, because averaging colour in sRGB space makes each level a little brighter
than the one above it.

**Suggested:** `Texture2D::GenerateMipMapsEXT()`, sRGB-aware, and change the doc comment from
"generate" to "allocate".

### A4. `AtmosphericSky` is undefined below the horizon and does not say so.

`setSunDirection`'s documentation says "straight down is midday; near horizontal is sunrise or
sunset". It says nothing about a sun that has *set*, and the model does not clamp: it keeps
integrating and returns a saturated red at the horizon shading through yellow to green further up.
In a night frame that arrives as a band of colour along every roofline against the sky.

`radiance()` clamps the **view** direction's upward component (`std::clamp(upwards, 0, 1)`); the sun
direction is only normalised.

**Suggested:** either clamp the solar elevation inside the model and document the floor, or add a
query — `isSunUpEXT()` — so a caller knows it has to paint its own sky. CNA City's workaround is to
stop drawing the sky below the horizon and composite night itself.

### A5. Only transparency reports why a pass fell back.

`RenderPipeline::getTransparencyFallbackReasonEXT()` exists and is exactly the right idea. SSAO,
SSR, motion blur, depth of field and the light shafts all depend on inputs the game must supply, and
`SsaoPass`'s own header says it "copies its input unchanged when any of them is missing". There is
no way to ask *why*.

So `settings.setSSAOEnabled(true)` without a `DepthNormalPrepass` produces a frame that looks
exactly like ambient occlusion that is not very strong. This is the one failure mode the layer says
it works hardest to avoid — "a pass that silently does nothing is the failure this layer works
hardest to avoid" (`docs/cnaext-getting-started.md` §5) — applied to everything except transparency.

**Suggested:** one `getPassFallbackReasonEXT(...)` covering every input-dependent pass, or a
per-frame list of passes that ran versus passes that copied through. `getStatistics()` is most of
the way there already.

### A8. Volumetric fog is enabled by a setting and configured by a method nothing can reach.

`RenderPipeline` constructs a `VolumetricFogPass`, adds it to the chain whenever
`settings.getVolumetricFogDensity() > 0`, and **never calls `setLight` on it** -- there is no
`volumetricFogPass_->setLight` anywhere in `RenderPipeline.cpp`, and no accessor that would let a
game do it either. `VolumetricFogPass::setLight(shadowMap, lightDirection, lightColour)` is public
and unreachable.

What that produces is not "no fog". The march runs 32 slices with no light direction and no shadow
map, and above the rooflines -- where the depth image has nothing in it -- it integrates scattering
against nothing and returns a band of red and green across the sky. It is only visible in weather
that has fog in it, which is why it took an afternoon to find: the clear-weather screenshots were
all clean.

**Suggested:** either have `RenderPipeline` forward the light it is already given in
`setShadowScene` to the pass, or expose `getVolumetricFogPassEXT()`. As it stands the setting is a
switch that turns on an artefact.

*Diagnosed by elimination: the band survives with light shafts off, survives with height fog on,
and disappears the moment the volumetric density goes to zero. At `--quality low`, which enables
none of the chain, it was never there.*

### A6. sharp-runtime's `Parallel::For` creates one operating-system thread per iteration.

`System::Threading::Tasks::Parallel::For` calls `std::async(std::launch::async, …)` per index and
waits every `MaxDegreeOfParallelism` of them
(`modules/threading-tasks/include/System/Threading/Tasks/Parallel.hpp`). For a loop that runs *once*
over a hundred thousand agents — which is what CNA City uses it for, generating the population —
that amortises away completely and it is the right tool.

For five loops running thirty times a second it is thousands of thread creations per second, and
the creations cost more than the work. CNA City's tick therefore uses its own persistent pool.

**Suggested:** back `Parallel::For`/`ForEach` with a persistent pool and hand each worker a
contiguous chunk. The public API does not have to change, and .NET's own implementation is
pool-backed for exactly this reason.

### A7. Widening the sharp-runtime component closure is undiscoverable from the outside.

sharp-runtime instantiates a component only when something asks for it. Anything outside
`CNA_SHARP_RUNTIME_DEFAULT_COMPONENTS` — `Threading.Tasks`, here — therefore has no target and no
include path, even though its directory is visibly configured in the build tree. The failure is a
missing header from a component you can see being added.

The fix is to set `SHARP_RUNTIME_COMPONENTS` **before** `add_subdirectory(../cnanext)`, which is
only discoverable by reading CNA's own `CMakeLists.txt`.

**Suggested:** a paragraph in the consumer documentation, or a
`cna_require_sharp_runtime_components(<Component>...)` helper that can be called after the
subdirectory is added.

---

## B. Ergonomics, not defects

- **B1. Post-process settings are in screen widths and do not say so.**
  `setChromaticAberrationStrength` clamps to `[0, 0.1]`. A caller reaching for "a quarter" gets
  0.1, which is a tenth of the frame and renders three visibly separated copies of the scene. The
  clamp is a good guard; the unit belongs in the doc comment. The same applies to
  `setSSAORadius`, which is a UV offset compared against normalised depths rather than a world
  distance — the stock default of 0.5 is half the frame and behaves as a global dimmer.

- **B2. `DebugDraw`'s batch has to be opened before anything is submitted.** `begin()` "starts a
  batch and forgets whatever the last one held", so shapes added before it are silently dropped —
  the opposite of `SpriteBatch`, where `Begin`/`End` bracket the draws but a `Draw` outside them
  throws. `clear()`'s "leaving the batch open" also implies a batch can exist before `begin`.
  A throw, or a note on `addLine`, would cost one line and save the next caller an hour.

---

## C. Not CNA. Recorded so nobody chases them into the engine.

### C1. The face winding convention — CNA documents it, this program had it backwards. Three times.

`modules/graphics/examples/frontface_winding_test.cpp` states the contract from the FNA source
rather than from any renderer's behaviour: under `RasterizerState::CullCounterClockwise`, **a
triangle that appears clockwise on screen is the front face**, and each cull enum names the face it
*removes*. In world terms a front-facing triangle's right-hand-rule normal points *into* the solid.

An upward-facing surface therefore has to be wound so its winding normal points **down**. Writing
it the other way — which is what "counter-clockwise is front-facing" leads you to write — produces
a surface visible only from underneath. It cost three defects here and none of them announced itself:

- every carriageway in the city was back-facing, so what showed through was the ground plane: a
  strip of grass down the middle of every street, with the lamp posts and street trees correctly
  placed on either side of it;
- every flat roof was missing, which is far harder to see — from four hundred metres up you look
  *into* the buildings, whose walls are culled from the inside, and what you get is the pavement
  between them. It reads as a roof. It survived a dozen aerial screenshots and was only found by
  tinting the roof material magenta and looking again;
- and then the whole metro, in a way the first two do not prepare you for. The rule flips sign with
  which side of a surface you stand on, so a *ceiling* — correct when its winding normal points up —
  is the mirror image of a floor, and every ceiling in the tunnel was written as though it were a
  floor. Worse, mirroring a correct quad about an axis produces an incorrect one, so the two sides
  of every tunnel, the platform edge and the wall opposite it, and one of the two end panels were
  each wound the wrong way *because their partner was right*. Four of the six faces of every tunnel
  were inside out and the symptom was a passenger on a train seeing the city through the wall.

The engine is not at fault in any of the three, but the third says something about the shape of the
mistake: it is not a fact you learn once. It has a sign that depends on which side of the surface
the viewer is on, and every mirrored copy flips it. The fix here was to stop writing vertex orders
at all — `AddFacet` in `CityGeometry.cpp` takes a point known to be inside the enclosed space and
derives the winding and the shading normal from it, which makes the class of mistake
unrepresentable rather than merely fixed.

The convention deserves to be repeated in `MeshBuilder`-shaped documentation wherever geometry is
authored, because the failure is silent in both directions.

### C2. `RenderPipelineSettings` defaults are not the problem.

Checked, member by member, in the header: every optional pass starts off — `hdrEnabled_`,
`bloomEnabled_`, `ssaoEnabled_`, `ssrEnabled_`, `dofEnabled_`, `fxaaEnabled_`, `colorGradeEnabled_`
and `shadowsEnabled_` are all `false`; `chromaticAberration_`, `filmGrainIntensity_`,
`lensFlareIntensity_`, `lightShaftIntensity_`, `motionBlurStrength_`, `heightFogDensity_` and
`volumetricFogDensity_` are all `0.0f`. An earlier version of this file blamed a non-zero default
for the colour band along the rooflines; the cause was A4.

### C3. `PbrEffect`'s ambient is applied exactly as it looks.

EasyGL's lit shader computes
`lightSum = uAmbientColor + (light0·NdotL0 + light1·NdotL1 + light2·NdotL2) · shadow + punctual`
and multiplies by the albedo, the same as `BasicEffect`. CNA City's first night was far too bright
and the ambient had to come down to about a third of what intuition suggested — but that is a
calibration against an ACES curve in a scene whose emitters dominate, not an engine property.
