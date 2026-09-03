# CNA City — audit and plan

Written 2026-09-03, after an audit of the whole repository against the code that is actually there.

Tasks are ordered by what a reader would notice first, and each carries an **acceptance test** that
can be checked rather than argued about. Items are ticked as they land.

---

## The audit's headline

**The README describes a program that does not exist yet.** Six subsystems are named there and none
of them is referenced anywhere in `src/`:

| Claimed in README | Reality |
|---|---|
| `ClusteredForwardEffect` + `ClusteredLightBuffer`, "so that thousands of street lamps ... are real lights" | Not used. And it directly contradicts `CNA-FINDINGS.md` A1, which says in the same repository that they *cannot* be. |
| `LodGroupEXT` | Not used; LOD is hand-rolled distance bands. |
| `ParticleSystem` for rain and snow | Not used; precipitation is this project's own instanced particles. |
| `GpuTimer` and per-pass timings | Not used. The sentence "a demo that claims to find bottlenecks has to be able to name them" is, as it stands, false. |
| `EnvironmentProcessor` → image-based lighting | Not used; ambient is a hand-tuned constant. |
| Depth of field, motion blur, colour grading | Named in the `RenderPipeline` list; only ever set to off. |

A demonstration whose own description overstates it is worthless as a measurement. Every row above
is either **implemented** or **corrected**, and the plan says which and why.

The audit also found one contradiction the repository can be caught in by itself: the README says
the lamps *are* real lights, `CNA-FINDINGS.md` says they cannot be. The findings file is right.

---

## P1 — Make the documentation true

- [x] **P1.1 GPU timing.** Enable `RenderPipeline::setGpuTimingEnabledEXT` and put the per-pass
  timings on the HUD behind a key. *Accept:* the HUD can show a per-pass millisecond breakdown of
  the post chain, or state plainly that this renderer has no timer queries.
- [x] **P1.2 Image-based lighting from the real sky.** `AtmosphericSky::radiance` is a static CPU
  function, so the sky can be sampled into a small cube without rendering one, turned into
  irradiance and prefiltered specular by `EnvironmentProcessor`, and given to
  `PbrEffect::setImageBasedLightEXT`. Rebuilt only when the sun has moved enough to matter.
  *Accept:* ambient colour tracks the sky through the day with no hand-tuned constant, and the
  rebuild cost is on the HUD.
- [x] **P1.3 `LodGroupEXT` for people and vehicles.** Replace the hand-rolled distance bands with
  the engine's own selector. *Accept:* the three person levels and the vehicle draw distance come
  from `LodGroupEXT::select`, and the frame is unchanged.
- [x] **P1.4 Correct what cannot be implemented.** `ClusteredForwardEffect` and `ParticleSystem`
  claims replaced with what the program actually does and why, pointing at `CNA-FINDINGS.md`.
  *Accept:* no claim in `README.md` names a symbol that `grep -r src/` cannot find.

## P2 — Options that do nothing

- [x] **P2.1 `--threads N` is ignored.** `SimConfig::threads` is never read; the `JobSystem` is
  constructed in `Simulation`'s constructor before any configuration exists. *Accept:*
  `--threads 1` halves throughput in `--bench` and the HUD reports the count.
- [x] **P2.2 `--fixed-weather` is ignored.** `SimConfig::randomWeather` is set by the parser and
  read by nothing, so the weather wanders off whatever was asked for within minutes. *Accept:*
  `--weather fog --fixed-weather` is still fog after a simulated day.

## P3 — Determinism above the tick

- [x] **P3.1 Decisions depend on the frame rate.** Trip decisions hash on `tick_`, so a machine
  drawing at 120 fps produces a different city from one at 60. *Accept:* the hashes take a
  quantised *simulated* clock, and two runs at different fixed steps produce the same trip
  decisions.

## P4 — Things a city has that this one does not

- [x] **P4.1 Traffic lights that change colour.** The phase is simulated and `SignalColour` exists,
  but the signal heads are drawn in one static colour, so the most legible piece of city machinery
  is invisible. *Accept:* a signal head is red, amber or green according to its own approach, and
  the change is visible from the street camera.
- [x] **P4.2 Parked cars.** Kerbside parking is most of what makes a street look inhabited, and its
  absence is why every road here reads as a bypass. *Accept:* parked vehicles line the kerb on
  local and collector streets, generated with the city and not simulated.
- [x] **P4.3 Pedestrian crossings.** Zebra markings on the approaches to signalised junctions.
  *Accept:* visible from the street camera, and aligned with the carriageway.
- [x] **P4.4 Pitched roofs on houses.** Every building in the suburbs is a flat-topped box, which is
  the one silhouette a suburb never has. *Accept:* `BuildingKind::House` gets a ridged roof.
- [x] **P4.5 Facade variety.** `Building::variant` is assigned and never read. *Accept:* it selects
  between facade tints so a terrace is not one colour repeated.
- [x] **P4.6 Snow that settles.** `Weather::snowCover` is simulated and nothing shows it.
  *Accept:* ground materials whiten with the cover.

## P5 — The follow camera should be able to follow a whole day

- [x] **P5.1 Keep the subject.** The camera abandons anyone who stays indoors for two and a half
  seconds, so "watch one citizen's entire day" is exactly what it cannot do. *Accept:* a key locks
  the subject; locked, the camera waits outside their door and picks them up when they leave.

## P6 — The tick path allocates

- [x] **P6.1 Per-tick allocations.** `RebuildCrowdGrid` allocates a 262 KB cursor array every
  sub-step, `Traffic::RebuildLanes` and `StepWalking` allocate every tick. *Accept:* the scratch
  buffers are members, and `--bench` at 100 000 agents is no slower.

## P7 — Dead code

- [x] **P7.1** `MeshData::AddPrism`, `Pathfinder::ResetStats`, `Pathfinder::cacheEntryCount` and
  `CityGeometry::bounds` are unreferenced. *Accept:* removed, or wired to something.

## P9 — Found while implementing P4

- [x] **P9.1 Volumetric fog draws a coloured band across the sky.** `RenderPipeline` adds the pass
  and never gives it a light, and exposes no way for a game to. Turned off, with the diagnosis in
  [`CNA-FINDINGS.md`](CNA-FINDINGS.md) A8.

## P8 — Honesty about arrivals

- [x] **P8.1 Drivers teleport on arrival.** `FinishTrip` moves the agent to the destination doorway
  from wherever the car happened to be, which for a vehicle abandoned to gridlock is a jump across
  the city. *Accept:* a driver who ends up more than a short walk from the door walks the rest.

---

## P10 — The underground and the buses

Added 2026-09-03, after "prosim vylepsi podzemi a autobusy". The two systems a passenger actually
travels on were the two least finished things in the program: the underground was geometry you
could see the city through, and the buses did not exist as a service at all.

- [x] **P10.1 The tunnel shell leaks daylight.** Four independent defects, each found by rendering
  the raw scene rather than by reading the code. (a) Every enclosed surface was hand-wound and a
  mirrored copy of a correct quad is an incorrect one, so exactly one wall of each mirrored pair
  was culled -- both tunnel sides, the platform edge, the wall opposite it and one end panel.
  (b) A box per polyline segment plus a wider box per station has a joint at every station, and two
  boxes meeting at an angle leave a wedge of open ground on the outside of the turn. (c) Geometry
  lands in the chunk containing its centre and may stick out of it, and a 400 m tunnel segment in a
  340 m chunk vanished whenever the chunk holding its middle left the frustum. (d) The sky lit the
  tunnel, because the shadow cascades are fitted to a frustum that is entirely underground.
  *Accept:* a screenshot from inside a train at any point on any line shows no sky and no city.
  Verified at six positions on four lines.

- [x] **P10.2 No caller may state a vertex order again.** `AddFacet` takes a point known to be
  inside the space being enclosed and derives both the winding and the shading normal from it.
  *Accept:* the winding trap that has now cost three separate bugs (roads, roofs, tunnels) is
  unrepresentable in the underground's geometry.

- [x] **P10.3 The follow camera cannot film a tunnel.** It stepped backwards along the subject's
  heading, which at a bend is ten metres into the earth, and it eased in real seconds while the
  world runs at sixty times real time -- so a subject on a 21 m/s train sat five hundred metres
  ahead of the lens. *Accept:* `--follow-metro` shows a carriage flank, not a distant pixel.

- [x] **P10.4 A bus was a body shape, not a service.** `VehicleKind::Bus` was handed to one private
  commuter in twenty-five and `PropKind::BusShelter` was scattered where no bus stopped.
  *Accept:* `BusNetwork` with stops, routes, a fleet and passengers; no private driver is ever
  given a bus; every shelter drawn is one a service calls at.

- [x] **P10.5 Nobody can watch a bus commute.** *Accept:* `--follow-bus`, the counterpart of
  `--follow-metro`, and a `GETS OFF AT` line in the follow panel.

- [x] **P10.6 The raw scene cannot be captured.** `F2` bypasses the post chain but there was no way
  to ask for that from the command line, and "is that white wash the geometry or the tonemapper" is
  the first question every lighting bug asks. *Accept:* `--no-post`. It is how P10.1(a) was pinned
  down.

### Known and not fixed

- **Buses are not in the car-following stream.** They obey the signals but do not queue behind
  cars, and cars do not queue behind them. Putting them in `Traffic` means giving each one a driver
  agent out of the population, which would distort every demographic number on the HUD; the honest
  alternative is a second vehicle class in the IDM arrays, and that is a larger change than this
  round of work. What is visible from the pavement -- a bus stopping at a red, pulling in, dwelling
  and pulling out -- is modelled; what is not is a bus blocking the lane it is stopped in.
