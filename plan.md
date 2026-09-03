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

---

## P11 — A test suite, and the five defects it found on the first run

Added 2026-09-03. The audit that produced this file missed the largest hole in the repository:
thirteen thousand lines of simulation with no test target, no test suite, and `static_assert` as the
only assertion mechanism anywhere in `src/`.

That matters more here than in an ordinary program, because this one is a benchmark. **A simulation
that quietly stops moving anybody gets faster.** Every defect in the table in
[`ARCHITECTURE.md`](ARCHITECTURE.md) did exactly that, and three of them — pedestrians who never
arrived, citizens who never left home, and half the population no longer making decisions — looked
like tuning rather than like breakage.

- [x] **P11.1 A test target.** GoogleTest, reused from `../cnanext/vendor/googletest` rather than
  fetched a second time. The simulation half of the program becomes a library (`cna-city-sim`) so
  the tests can link the city without linking a renderer — the same split `--headless` already
  draws. *Accept:* `ctest` runs, and 71 cases across twelve suites pass.

- [x] **P11.2 Regression tests for every defect on record.** Named for the defect rather than for
  the function, because that is what they are for: `PedestriansActuallyArriveAtALargeTimeScale`,
  `MostOfTheCityLeavesHomeDuringTheMorningPeak`, `TheDecisionStrideReachesEveryAgent`,
  `NoTwoVehiclesOccupyTheSamePlaceInTheSameLane`, `DriversDoNotTeleportToTheirDestination`,
  `PassengersBoardRideAndAlightRatherThanWaitingForever`, `DanglingStubsArePrunedAway`,
  `BlocksAreBoundedAndPlausiblySized`, `NoPrivateDriverIsGivenABus`.

### What it found, on the first run

Five live defects, none of which anyone had noticed:

- [x] **P11.3 Every pitched roof in the city was inside out.** The same class of mistake as the
  flat roofs, and far harder to see: from a street-level camera you look *up* at a two-storey
  house, and an inverted slope is visible from underneath. The suburbs were correct in every
  eye-level screenshot and missing from every aerial one, leaving a scatter of gable slivers where
  the roofs should have been. Found by a unit test, confirmed by tinting the tile material magenta
  and looking down at the city.

- [x] **P11.4 Vehicles were held at a negative distance along their segment.** The stop line is set
  back by the half-width of the road being crossed, and the graph is cut at every intersection, so
  a short link between two close junctions is shorter than its own stop line is deep. Such a
  vehicle was parked six metres *before* the start of its segment — which is to say in the middle
  of the junction it had just left.

- [x] **P11.5 Buses drove through each other, and then deadlocked when that was fixed.** The
  look-ahead was applied and then overwritten by the stop-approach logic on the next line; the
  repaired version treated a bus clearly *behind* as ambiguous and had two buses each yield to the
  other, nine metres apart, for the rest of the day. Both are now tested: zero same-route
  overlaps, and no bus stationary for more than 90 s without dwelling or waiting at a signal.

- [x] **P11.6 The same seed produced a different city at a different frame rate.** Three separate
  causes, and the claim in the README had been false since it was written. The decision pass ran
  when an accumulator crossed a threshold, so it happened at t = 2.0 at one step size and t = 1.5
  at another; `WorldClock::Advance` added a float to a float, and 2 400 additions of 1/7200 land
  two seconds away from 1 200 additions of 1/3600; and the weather's fog term reads the hour of day
  *inside* its own exponential smoothing, so it does not compose under subdivision.

- [x] **P11.7 The same seed produced a different city on a different number of threads.** The list
  of citizens who want to leave is gathered in parallel with an atomic index, so its *order* is
  whichever worker got there first — and planning consumes it in order under a budget. `--threads`
  was deciding which citizens travelled.

P11.6 and P11.7 together are why `Step` is now a fixed-timestep loop: the frame's elapsed time is
banked and the world advances in whole ticks of `kMovementStep`, with the decision period an exact
multiple of it. A tick is identical whatever asked for it, and the only thing a frame rate can
change is how many run in one call. That is also the precondition for everything else worth doing
here — checksums, replay and snapshots are all statements about a world that advances the same way
twice.

- [x] **P11.8 The mode lists were stale by the time anyone could read them.** They were collected
  before movement, and movement is what changes a mode, so the renderer drew citizens who had gone
  underground a moment earlier as though they were still on the pavement.

### Known and not asserted

- **Buses on converging routes still overlap occasionally.** Along a route the look-ahead is exact
  and the suite asserts zero overlaps. Across routes it is not: two services meeting on a shared
  arterial from different streets see each other late, a few dozen moments a simulated hour, never
  closer than four metres. That residual is bounded by the test rather than asserted away, and
  closing it properly means putting the fleet in the IDM arrays.

---

## P12 — Determinism as a property rather than a claim

Added 2026-09-03, after P11 made the tick fixed-length. The README had said "the same seed is
always the same city" since it was written, and P11 found three ways in which that was false. A
sentence that has been wrong three times needs a check rather than a rewrite.

- [x] **P12.1 A digest of the world, split five ways.** `--checksum` simulates for `--simulate`
  and prints CITY / AGENTS / TRAFFIC / TRANSIT / WORLD / FINAL. Five rather than one because a
  mismatch should be a lead: a city that differs means the generator moved, agents alone means the
  schedule or the steering did. Floats are quantised to a centimetre before hashing, so the check
  fails on a defect rather than on a compiler flag. *Accept:* two runs of the same command print
  the same six lines.

- [x] **P12.2 The check checks itself.** `--checksum` re-runs at half the step size and on a
  different number of worker threads and reports whether those agreed. A determinism claim that is
  only ever exercised one way is one that has already been broken.

- [x] **P12.3 Record and replay.** `--record FILE` writes the *input* -- seed, configuration, tick
  count, and the moments somebody changed the weather or wound the clock -- and nothing else,
  because everything else is recomputable. A simulated day of a hundred thousand citizens is about
  a kilobyte. `--replay FILE` re-runs it, compares a digest at checkpoints, and stops at the first
  disagreement naming the tick and the component. `--replay FILE --threads N` re-runs on a
  different number of workers. *Accept:* a recorded run reproduces; a tampered checkpoint is caught
  at the right tick and attributed to the right component; a changed seed is caught before a single
  tick runs.

### What it found

- [x] **P12.4 The arrival queue depended on which worker thread finished first.** The list of
  citizens who arrived somewhere this tick is gathered in parallel with an atomic increment, so its
  order was whichever worker got there first -- and an arrival joins the *back* of a platform or
  bus-stop queue that a vehicle drains from the *front*. Which of two citizens got on a full train
  was decided by thread scheduling. The same root cause as the planning-queue defect in P11.7, in
  the second of the two places it occurs.

  It needed a hundred thousand citizens and eight simulated hours to show: a two-hour run
  reproduced perfectly. It was found by recording a run and replaying it, which named tick 50 004
  and said the divergence was in the agents.

### Known and not asserted

- **There is no unit test for P12.4, deliberately.** Comparing two runs only catches an ordering
  race when the scheduling differs at a moment that changes an outcome, which needs a full vehicle.
  Measured: at fifty thousand citizens over ninety simulated minutes it caught three times in six;
  at twenty and thirty thousand funnelled through one metro line and one bus route, not once in
  twelve. A test that passes half the time when the bug is present teaches people that a failure is
  noise. The guard is `--checksum` at full scale, where the race is reliable, and it belongs in CI.

- **Only two kinds of interactive event are recorded**, the weather and the clock, because they are
  the only two that change the simulation. Camera moves, overlays and the quality dial do not, and
  a replay that recorded them would be a screen recording with extra steps.
