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

- **Buses are not in the car-following stream.** *(Closed in P16 -- they now share the road's
  occupancy model, which is the half of this that was visible.)*

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
  draws. *Accept:* `ctest` runs, and 71 cases across twelve suites pass. (That was the count P11
  delivered; the suite is 102 cases across 17 suites as of P20, and every later section says what
  it added.)

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

---

## P13 — Snapshots, so a measurement does not start with a wait

Added 2026-09-03. Measuring the morning peak means simulating up to the morning peak first: at a
hundred thousand citizens that is twenty-five seconds of warm-up before the first number, paid
again for every scale, every renderer and every run.

- [x] **P13.1 `--save` and `--load`.** A snapshot stores what cannot be recomputed -- the
  population, the traffic, both fleets, the clock, the weather, every generator's state and the
  queues on the platforms -- and a *digest* of the city, which is a pure function of the seed. A
  snapshot taken against a generator that has since changed is refused rather than loaded into a
  world whose roads have moved under its traffic. *Accept:* loading takes 0.27 s against 25 s of
  warm-up, and a restored world has the same future.

- [x] **P13.2 One traversal for reading and writing.** `Archive` walks the same code in both
  directions, because every save format with a `Write` and a matching `Read` eventually has a field
  in one and not the other -- and the symptom is a file that loads into a plausible world with
  everything after that field shifted by four bytes.

- [x] **P13.3 A scenario set.** `scripts/make-scenarios.sh` builds empty-night, morning-rush,
  evening-rush, rain-gridlock and metro-peak. Twenty megabytes each and gitignored: they are
  regenerable, and a git history is not the place for what can be recomputed.

- [x] **P13.4 `--bench --load`.** Measures one scenario rather than sweeping from an empty city.
  A snapshot pins the population, so a sweep and a snapshot are mutually exclusive and the caller
  is told rather than surprised.

### What it found

Building the `--checksum` self-check meant initialising two simulations to compare, and nothing had
ever done that before. Three things did not survive it:

- [x] **P13.5 `City::Generate` did not clear what it generated.** Every stage cleared its own
  output except one: park planting appends to `props_` while the blocks are being built, so
  `GenerateProps` deliberately does not clear it -- and nothing else did either. Generating twice
  into the same `City` produced two of every lamp post and two of every tree, some of them in
  places that were no longer parks.

- [x] **P13.6 `Simulation::Initialize` reset three counters and left four.** The decision pass, the
  step accumulator, the planning rotation and the day-rollover marker kept whatever the previous
  run had left them at.

- [x] **P13.7 `Traffic::Build` left the signal cycle where it was.** The per-junction offsets are
  hashed from the node index and were always reproducible; it was the clock they are measured
  against that carried over, so a rebuilt city had nine junctions at a different point in their
  phase on the first tick.

All three were invisible for as long as nothing built a city twice, which nothing did until this.

---

## P14 — A benchmark that leaves something behind

Added 2026-09-03. A number printed to a terminal is a number nobody has next week, and comparing
two machines meant comparing two scrollbacks.

- [x] **P14.1 `--report DIR`.** Writes `system.json`, `simulation.csv`, `memory.csv`,
  `rendering.csv`, `passes.csv` and one `report.html`. The page is self-contained -- the charts are
  inline SVG rather than a library from a CDN, because an artefact whose purpose is to be kept and
  opened later is usually opened somewhere without a network, and a page that fails that way fails
  silently with blank graphs. *Accept:* every file parses, the page contains no `http`, and it
  still renders with no data in it.

- [x] **P14.2 A spread, not a number.** Each scale is measured `--repeat` times; the fastest run is
  reported and the spread beside it. Two runs of the same build do identical work, so the
  difference between them is whatever else the machine was doing -- the minimum is the closest
  estimate of what the program costs, and the spread is what says whether the estimate is worth
  anything. Measured on this repository: ±0.02 ms at a thousand agents on an idle machine, ±3.73 ms
  at a hundred thousand on a busy one. A report that printed the second as fact would be
  indistinguishable from a 50% regression. The page flags a spread wider than 15% of the mean.

- [x] **P14.3 The report names what it measured.** Renderer, compiler, build type, thread count,
  seed, the city's digest and the load average at the time. A benchmark that does not say what it
  ran cannot be compared with anybody else's, and one that does not say the machine was busy
  invites a false conclusion.

- [x] **P14.4 The rendering half.** `--report` with a device drives the camera through a fixed
  tour -- a city overview where the shadow cascades dominate, the downtown skyline, a street at eye
  level where the simulation is the larger half, and a signalised junction -- warming each up for
  as many frames as it then measures, so a shader compile does not become the measurement. It runs
  *after* the simulation sweep rather than beside it, because the two compete for the same cores
  and a frame time measured while a hundred thousand citizens are simulated on every other thread
  is a measurement of the scheduler. `passes.csv` comes from CNA's own timer queries, and shows
  what a CPU-side stopwatch cannot: SSAO appears only in the street-level views, because ambient
  occlusion switches itself off above roof height. `--headless --report` writes the simulation half
  alone.

---

## P15 — Comparing runs, and the shape of comparing renderers

- [x] **P15.1 `--compare A B [...]`.** Reads report directories back in and writes one page with
  them side by side. Two uses want the same page: the renderer comparison this project exists to
  make possible, and the one that comes up daily -- did this commit change anything?

- [x] **P15.2 A difference is measured against the noise, not against zero.** A change smaller than
  the spread of either run is labelled *within noise*. A benchmark that reports every wobble as a
  regression is one whose regressions get ignored, and this project has already produced a 30%
  "difference" that was nothing but a busy machine.

- [x] **P15.3 Reports of different cities are refused loudly.** Two runs from different seeds or
  different generators are two different workloads; the city digest in each report is what catches
  that, and putting their numbers in one table without saying so is the most confident possible way
  to reach a wrong conclusion.

- [x] **P15.4 `scripts/compare-renderers.sh`.** The sweep across CNA's renderers. Deliberately not
  run here and deliberately not wired into anything: each renderer is a separate configure and a
  full rebuild of CNA, minutes of compilation and hundreds of megabytes of objects. The script
  reuses the workspace's one shared scratch build directory and measures the renderers one after
  another, because the alternative -- a build tree per renderer -- is a directory nobody ever
  deletes.

### The measurement still to take

The comparison machinery is done and tested; what has not been done is spending the disk and the
compilation on a second renderer. That is a decision about this machine rather than about the code,
and the script makes it one command when somebody wants it.

---

## P16 — One occupancy model for the road

The thing left over from the buses: they obeyed the signals but nothing else. A bus did not queue
behind a car, a car did not queue behind a bus, and a bus standing at a stop was invisible to the
lane it was standing in.

The cause was two models of the same asphalt. `Traffic` kept vehicles in per-lane buckets sorted by
distance along the segment; `BusNetwork` kept buses as an arc length along a polyline, with an
all-pairs scan of its own to stop them driving through each other. Neither could see the other,
because neither was written in the other's terms.

- [x] **P16.1 A bus knows where it is on the road, not only on its line.** Routes carry the road
  node behind every point and the segment between consecutive points, so an arc length maps to
  (segment, direction, offset) exactly rather than by proximity.

- [x] **P16.2 One structure, published to and read from by both.** `Traffic::SetObstacles` puts the
  buses into the lane buckets, so the cars behind them queue -- and `Traffic::GapAhead` reads the
  same buckets back, so the buses queue too. The all-pairs bus-versus-bus scan is deleted: it falls
  out of the shared structure, and it never could see a car anyway. *Accept:* a screenshot of a
  queue behind a bus at a stop, and a test that most cars close behind a dwelling bus are slowing.

- [x] **P16.3 The ordering is the unification.** The buses move *after* the traffic. Their
  positions go in before `Traffic::Step` rebuilds the lanes, so the cars queue behind them; the
  buses then read those same buckets, so both halves work off one rebuild they agree on. Stepping
  the buses first read the buckets from the tick before, which did not contain them -- which is
  what the first attempt did, and it looked like the whole thing had not worked.

- [x] **P16.4 Over the junction as well.** Lane buckets are per segment, so a bus at the end of one
  cannot see the queue that starts at the beginning of the next -- the same blind spot the cars
  have, which their junction pass handles. One extra query when the end is close, in the *next*
  leg's lane rather than the current one, because a route turning from a collector onto an arterial
  changes which lane is the kerbside one.

### The residual, and why it stops here

Same-lane overlaps went from hundreds a run to single figures over a third of a million bus-ticks.
What is left is inherent: when a bus crosses a junction its position jumps to the start of the next
segment, and if something is standing there the two are momentarily inside each other. A car in
that position is pushed back by the traffic model's own negative-gap correction; a bus cannot have
one, because `Traffic` does not own where it is. A correction of the bus's own was tried and made
it slightly worse -- pushing a bus back can put it inside whatever is behind.

Closing it properly means moving the fleet into the IDM arrays, with the junction pass asking the
bus route for the next segment instead of asking a driver's path. That is a bigger change than the
one this bought, and the test bounds the residual rather than pretending it is not there.

---

## P17 — Heatmaps: what the city is costing, painted on the city

The overlays showed what the simulation *is* -- the road graph the planner sees, the live routes.
Nothing showed what it *costs*, so "where is the frame going" and "where is the congestion" were
questions answered by reading numbers off a HUD and guessing at a place.

- [x] **P17.1 A layer on its own key.** `F4`, not another `Tab` stop, because the two answer
  different questions and are worth seeing at once. Also `--heatmap NAME`, because a screenshot has
  no keyboard.

- [x] **P17.2 Four of them, each measured rather than guessed.**
  *traffic*: mean speed per segment against that road's own limit -- absolute speed would paint
  every suburb red. *density*: people and vehicles per 100 m cell, a size chosen because finer is
  speckle and coarser smears a junction across a district. *render*: triangles per **visible**
  chunk, since a culled one costs nothing. *paths*: route searches per district.

- [x] **P17.3 The path map counts misses, and forgets.** A cache hit costs nothing and happens
  wherever the last person went, so counting queries would map popularity rather than work. And it
  decays on a ten-second half-life measured in *simulated* time -- a cumulative count is uniform by
  lunchtime, and a decay measured in frames would make the picture depend on the frame rate, which
  the rest of this loop spent P11 and P12 making impossible.

- [x] **P17.4 Every heatmap carries its scale.** The legend line names the quantity and the peak.
  Without one it is a picture of where the red is, and the same city looks alarming or fine
  depending on what the brightest cell happened to be.

The colour ramp is blue-green-yellow-red rather than a rainbow or a single hue. A rainbow puts its
brightest band in the middle and makes a mid value read as the extreme; a single hue makes the
difference between "busy" and "stopped" a shade of the same colour.

---

## P18 — A million citizens, and what actually gives first

The question was which of RAM, the crowd grid, the path cache, the decision pass, the walking pass,
the rendering or the route pool fails first at ten times the headline population. The answer is
none of them.

A million agents runs. Nothing throws, nothing is exhausted, nothing degrades non-linearly. What
happens is that the tick gets proportionally dearer and one *policy* -- not a data structure --
stops scaling with it.

- [x] **P18.1 Measure it.** 1 000 to 1 000 000 in the 3.3 km city, and 1 000 000 again in a 9 km
  one that can actually house them. The scaling sweep grew two columns for the question:
  `deferred`, the peak number of citizens who wanted to leave and were not planned that pass, and
  `poolfull`, how many times the route pool ran out of slots.

- [x] **P18.2 What gives: the planning budget, and it was a constant.** `poolfull` is zero at every
  scale -- the pool is sized from the population and 400 000 slots at a million agents is 102 MB
  that never runs out. Memory is 191 MB reported and 255 MB resident, which is linear. The crowd
  grid is a hashed 65 536-bucket structure over the people outdoors and never became the cost.

  The budget did. `kPlanBudgetPerTick` was a flat 320 trips per pass whatever the population, so
  the *share* of demand served fell with scale: 35% of the peak at a hundred thousand, 3.5% at a
  million. Nothing broke; the city simply took ten times as long to get moving, which turns a
  benchmark into a measurement of a queue. It is now 320 per hundred thousand citizens, with a
  floor that leaves the hundred-thousand case byte-identical -- the served share is 35% at both
  ends, and the peak deferred at a million fell from 8 735 to 5 855.

- [x] **P18.3 The city is the other limit, and it is not a defect.** The default 3.3 km city has
  homes for 133 911 people. A million of them run in it perfectly well and every one has somewhere
  to live, seven and a half to a dwelling. `--size 4500` gives a 9 km city that houses them
  properly; generation scales close to linearly with area (26 ms at 1 650, 174 ms at 4 500), and
  the *simulation* is dearer there than in the small one because the trips are longer -- 185 918
  people in transit at the peak against 104 657, and a metro tick of 9.0 ms against 1.5.

### What a million costs

Measured on the machine in [`ARCHITECTURE.md`](ARCHITECTURE.md) §7. The structural columns are
exact at any load; the millisecond ones were taken while the machine was not idle and the sweep
does not repeat, so they are an upper bound rather than a best-of.

| agents | mean | p99 | decide | walk | crowd | memory | peak out | deferred | pool full |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 000 | 0.47 ms | 2.07 | 0.01 | 0.02 | 0.13 | 7.7 MB | 104 | 0 | 0 |
| 10 000 | 0.89 ms | 2.93 | 0.08 | 0.23 | 0.16 | 9.4 MB | 844 | 0 | 0 |
| 100 000 | 3.93 ms | 9.52 | 0.58 | 1.09 | 0.54 | 25.9 MB | 8 913 | 597 | 0 |
| 250 000 | 10.83 ms | 48.01 | 1.44 | 3.07 | 1.44 | 53.4 MB | 25 475 | 1 455 | 0 |
| 500 000 | 23.61 ms | 67.34 | 2.84 | 6.53 | 4.12 | 99.2 MB | 52 391 | 2 929 | 0 |
| 1 000 000 | 47.90 ms | 93.46 | 5.24 | 14.39 | 10.29 | 191.0 MB | 104 592 | 5 855 | 0 |
| 1 000 000, 9 km city | 76.71 ms | 144.13 | 10.52 | 19.54 | 12.07 | 198.9 MB | 185 918 | 8 759 | 0 |

**A thousand times the agents costs a hundred and three times the tick**, and the tick is
proportional to the people *outdoors* rather than to the population: 104 592 of the million are in
the street at the peak, and 8 913 of the hundred thousand, which is the same tenth. The route
cache is what keeps the ratio near one rather than above it, and the crowd pass is what pulls it
above one at the top -- 0.13 ms to 10.29 is eighty times for a thousand times the agents, and it is
the only line in the table growing faster than the travellers do.

At a million the simulation alone is 48 ms, so the demo is simulation-bound at about twenty ticks a
second before a frame is drawn. Nothing prevents it running; it is simply no longer a real-time
program at that size, which is a useful thing for a benchmark to be able to say precisely.

---

## P19 — Does overlapping the step with the draw help? Measure it, do not assume it

The architecture runs the simulation and the draw one after the other, and has always said so. The
question is whether overlapping them is worth the complexity -- asked as an experiment with a
switch, rather than answered by implementing threading because threading sounds good.

- [x] **P19.1 `--frame-model serial|pipelined`.** Serial is the default and is unchanged.

- [x] **P19.2 No snapshot, because the ordering makes one unnecessary.** `CollectVisible` is the
  last thing in the frame that reads the simulation; everything after it draws from the instance
  buffers it just filled and from the static city, which has not changed since start-up. So the
  step is started *after* the collect and joined before the overlay and the HUD, which read the
  simulation again. A snapshot of a hundred thousand citizens would have cost more than it saved.

  Four floats are the exception and are copied rather than shared: the night level, the wetness,
  the snow cover and the daylight, which the draw reads after the launch and the step writes. Four
  unsynchronised floats is a small race and a real one.

- [x] **P19.3 Instrument the thing that answers the question.** The pipelined model reports the
  *blocked* time -- how much of the step the draw failed to cover. That number is what says whether
  an overlap happened, and it is far less sensitive to what else the machine is doing than a frame
  time is, because it compares two things inside the same frame.

### What it measured, and what it could not

The overlap is real and available, and this is the part that measured cleanly. Across four separate
runs the blocked time is **0.008 to 0.3 ms against a step of 3 to 5 ms** -- the draw covers
essentially the whole simulation, and the step leaves the critical path completely. Under load it
rises to 8-10 ms: the two halves are then competing for the same cores rather than overlapping on
them, because the simulation already uses every core through its own pool.

That the blocked figure held across four runs while the frame times did not is the reason to
instrument it. It compares two things happening inside the same frame, so it survives conditions
that make the frame time meaningless.

What this machine could not answer is what that is worth in frames per second. Three interleaved
pairs of runs gave gains of +61%, -11%, -19% and -20% by viewpoint, while three runs of the
*identical* serial configuration gave 13.0, 37.7 and 40.1 ms for the same viewpoint. The
within-mode spread is three times the between-mode difference.

Waiting for a quiet machine was tried and is not enough: a fifth pair, gated on the one-minute load
average dropping below 2 before it started, still measured -117% on the city overview -- the load
was 4.5 by the time it finished, and the pipelined run's *draw* had gone from 14.6 ms to 22.8 while
its blocked time stayed at 0.18. The comparison needs a machine that is quiet for the whole of it,
not one that is quiet when it starts. The honest answer is that the experiment is built and
instrumented and the frame-rate question is still open.

**One thing it did establish, which was not the question.** In the serial model the simulation cost
per frame is self-reinforcing: the simulated interval is the real frame time times the time scale,
so a slow frame simulates more, which makes it slower. The measured step ranges from 3 ms to 21 ms
across runs of an identical workload for that reason alone. Pipelining breaks the loop, and that
may matter more than the overlap does -- a frame-rate floor that holds up under load is worth more
than a few per cent at the top.

---

## P20 — Correctness hardening, from an outside audit

Four defects, all found by somebody reading the code rather than running it, and all in work from
P19 or in documentation that had drifted behind it.

- [x] **P20.1 The pipelined model still had a data race, and my check for it was blind.** The fix
  in P19 hoisted four reads of the clock and the weather out of `Draw`'s own body. `DrawSkyOverlay`
  and `DrawStaticCity` are called between the launch and the join and read five more --
  `Daylight`, `StreetLightLevel`, `cloudiness`, `wetness`, `snowCover` -- and `cloudiness` was not
  among the four. The grep that reported "none" only looked at the lines of `Draw` itself.

  Fixed with a `FrameEnvironment` captured before the launch and passed down. A struct rather than
  five parameters, and rather than another comment: a type those functions cannot do without is a
  check that survives the next person, which a comment saying "nothing here reads the simulation"
  demonstrably did not.

- [x] **P20.2 `simMs_` meant two different things.** In the serial model it was the step; in the
  pipelined one it was launch plus join -- the part of the step the draw failed to cover. Both are
  worth having and neither should be called "SIM": a 13 ms step read as 0.0. Split into
  `STEP` (wall), `BLOCKED`, `HIDDEN` and `OVERLAP %`, with the wall measured inside the job because
  the frame cannot see it. `rendering.csv` carries both, and the frame model is now in
  `system.json` -- with `--compare` refusing to put two models side by side without saying so.

- [x] **P20.3 `FrameWorker` could not report a failure.** An exception leaving a `std::thread`'s
  entry point is `std::terminate`, so a step that threw would kill the pipelined build outright
  where the serial one reports it. The worker keeps an `exception_ptr` and `Wait` rethrows it, so
  both models fail the same way. Six tests, including that a failure does not poison the next job
  and that the destructor does not throw when nobody was waiting.

- [x] **P20.4 The documentation had drifted.** README claimed 71 cases across twelve suites (102
  across 17) and seven capability gaps (eight, A1 to A8); `framework-fixes/README.md` said nine
  findings and "the five with no patch" while listing four. All re-derived from the source rather
  than edited by hand: three patches, one note, four left open, which is all eight.

## What comes next, and what deliberately does not

Agreed after the audit, in order:

1. **A soak test.** Several simulated days, asserting that nothing accumulates: no growing count of
   routes nobody is following, no route slots leaked, no passengers waiting forever, no memory
   growth, no permanent gridlock, and a population that actually goes home in the evening. Failures
   that only appear after hours of simulated time are exactly what a benchmark should be able to
   find and what nothing here currently looks for.
2. **Freeze and tag a baseline**, so future CNA versions are measured against a fixed point.
3. **Wait for the current CNA work to land**, then run the renderer matrix that
   `scripts/compare-renderers.sh` already exists for.
4. **Validate the framework patches separately**, against whatever `next` is by then. A6 in
   particular changes threading semantics and leaves the `ParallelLoopState` and `ForEach`
   overloads untouched; it should not go in on this project's say-so.

What is deliberately **not** next: schools, police, hospitals, an economy, trams added for the sake
of trams, or ten million citizens for the sake of a bigger number. The million already answered the
question worth asking, and answered it well -- the first thing to stop scaling was not a data
structure but a constant scheduling policy. Another subsystem earns its place only by presenting
CNA with a *kind* of load it has not seen.

---

## P21 — A soak test, and the five defects it found

Not "run it for an hour and see whether it crashes". The failures a benchmark has to worry about do
not crash: a route slot that is never given back, a passenger nobody ever picks up, a queue that
grows by four every morning and shrinks by three every evening. Each of those is invisible in a
thirty-second run, fatal in a long one, and raises no signal at all. They just make the numbers
slowly stop being about a city.

- [x] **P21.1 Structural invariants**, asserted at every checkpoint rather than watched. The ones
  worth having are the ones whose failure is silent: cross-links that must round-trip, sets that
  must partition, conservation laws counted from both ends, occupancy against capacity, and no two
  vehicles in one place. Half the tests for them deliberately break the world first -- a checker
  that has only ever been seen passing is a checker nobody should believe.

- [x] **P21.2 Accumulation**, as drift per simulated day with the first day discarded as warm-up.

- [x] **P21.3 Recovery**: what has to come back down. The planner's backlog, the traffic, the
  queues, the population.

- [x] **P21.4 Both frame models over the same slices**, digests compared at every checkpoint. They
  agree. The comparison is per checkpoint rather than at the end because "they differ after three
  days" is a sentence with nowhere to go and "they differ at 08:00 on day one, in the traffic
  digest" is the first line of a bug report.

- [x] **P21.5 A snapshot taken mid-run**, reloaded, and stepped alongside for a simulated hour
  before being compared. Comparing at the moment of loading only proves the file round-trips the
  numbers a digest looks at.

### Measuring a trend is harder than it looks

The obvious method is wrong and a test says so. A pure sine of period 24, sampled hourly over
exactly three days, has a least-squares gradient of **-3.5 per sample**. Whole periods cancel in the
mean and do not cancel in the covariance with time, so a city's daily rhythm produces a gradient of
its own whose sign depends on nothing but the hour the run started at. A leak detector built on the
raw gradient reports a leak on half the runs and hides one on the other half.

Subtracting each sample's hour-of-day mean is the obvious repair and is wrong more quietly: it
removes part of the drift along with the cycle, and reads a real twelve-a-day leak as nine. Whole-day
means, fitted through, are exact -- twenty-four uniform samples of anything with a twenty-four-hour
period sum to zero whatever hour the run began at.

### What it found on its first run

Five defects, none of which crashed anything, and every one of which was invisible in the numbers
the program already printed.

1. **Queue entries leaked whenever somebody gave up waiting.** The give-up rule set the mode and
   planned a walk; it never took the citizen out of the stop queue. The queues are compacted only
   when a vehicle dwells at that particular stop, so entries survived indefinitely -- and a citizen
   who gave up at one stop and later joined the queue at another was in two queues at once, where
   the next bus at the first stop could board them and place them onto itself from across the city.
   *457 of 3000 citizens were permanently "waiting" at three in the morning.*

2. **The give-up rule fired on people who had already boarded.** It iterates the mode list
   collected at the end of the previous tick, and the boarding pass has already run -- so somebody
   in that list may be sitting on a bus by now. Walking them off it without telling the bus leaves a
   seat occupied by nobody for the rest of the run. It needs the timer to cross its threshold in the
   very tick they board: *one citizen in twenty thousand per day*, which is often enough to matter
   over a week and rare enough that only a conservation count was ever going to find it.

3. **`FinishTrip` cleared the metro trio and not the bus one.** Nothing was wrong with the three
   lines that were there; the buses simply arrived later and the matching lines were never written.
   A citizen reached their own front door still holding a bus.

4. **Two buses on one piece of kerb were filed under different lanes.** The lane came from the
   lateral offset interpolated *along* a leg -- the right number for drawing a bus, the wrong one
   for filing it. A route leaving a narrow street for a wide one has an offset sliding from about
   2.5 m to about 5, crossing the 3.4 m lane boundary in the middle of a leg, and the occupancy
   structure is bucketed per lane. The two buses were invisible to each other, drove through one
   another, and stopped in the same place -- where each reads the other as zero metres ahead and a
   gap-based follower model has nothing asymmetric left to separate them with. The same blindness
   exists on the leg that closes each loop, which the router leaves without a road segment at all.
   *Whole routes finished the day stacked on a single metre of road, with fifteen passengers aboard
   who would never arrive anywhere, while every invariant still held -- because a stationary bus is
   not a broken one.* Fixed by taking the lane from the leg (the same leg that decides the segment,
   so the two cannot disagree) and by restoring a same-route following constraint, per route rather
   than all-pairs, which the road-occupancy optimisation had quietly dropped.

5. **The bay check measured the wrong point.** Taking a stop teleports the bus onto the stop's exact
   point -- forwards by up to a metre, or backwards by up to twenty when it has overshot -- so the
   question is whether the point it is *about to* occupy is free. It asked whether the point it
   currently occupies is free, so a bus sixteen metres past the stop passed the check and then
   jumped backwards on top of the bus standing in the bay.

### Two things I got wrong on the way, and what caught them

Worth recording, because both were fixed by measurement after a plausible story failed.

- I diagnosed the coincident buses as a bay-check problem twice -- once as a stale in-tick list,
  once as "dwelling" being the wrong predicate for "occupying the bay". Both are real defects and
  both are fixed above. **Neither produced the state I was looking at.** Instrumenting the snap
  showed zero buses ever snapping within two metres of another; the actual cause was the lane
  index, three hypotheses later.

- I then added a stall ceiling: a bus standing still for a minute creeps regardless of the gap,
  by analogy with the red-light hold that already has one. It broke the deadlock and **caused
  merges of its own** -- catching the tick where two buses first came within two metres showed one
  creeping into the other. It was removed rather than tuned. A rule that can produce the state
  that causes the disease is not a cure for it, and with the lane fix in place nothing needs it.

### What is still wrong, and is not fixed here

The command line's defaults at `--size 620` put **ninety-three buses over the twenty stops of a
1.2 km city**. That configuration no longer deadlocks and no longer strands anybody in a queue, and
its buses are never coincident -- but roughly fourteen riders and nineteen drivers still never
arrive anywhere overnight. The evidence points away from the buses: the drivers are cycling through
red lights rather than standing still, and the gridlock give-up asks whether a car has been
*perfectly still* for four minutes rather than whether it has *made any progress*, so a slow crawl
resets it forever.

### The run that answers the question

Three simulated days, 25 000 citizens, 72 checkpoints, both frame models in lockstep:

```
INVARIANTS  every check held at all 72 checkpoints
FRAME MODEL serial and pipelined agreed on every digest
SNAPSHOT    saved mid-run, reloaded, and carried on identically
ACCUMULATION over 48 checkpoints after the warm-up day
    route slots in use               mean     845.08     -0.833 per day   (allowed +20.000) ok
    queues at platforms and stops    mean      32.67     +4.583 per day   (allowed +25.000) ok
    simulation memory (MB)           mean      12.10     +0.000 per day   (allowed +1.000) ok
    path cache (MB)                  mean       6.25     +0.000 per day   (allowed +0.500) ok
    resident set (MB)                mean      70.47     +0.000 per day   (allowed +4.000) ok
RECOVERY
    route pool never exhausted       ok      0 times
    planner backlog clears           ok      peak 0, 0.0 overnight
    traffic clears after the peak    ok      peak 165, 0 overnight
    the city goes home at night      ok      100.0% indoors at 03:00
    nobody is left waiting overnight ok      0 people still waiting at 03:00
    day and night keep cycling       ok      daylight ranged 0.00 to 1.00

SOAK PASSED
```

Three of the five accumulation figures are exactly zero and the route pool drifts *downwards*. The
one worth naming was **the queues at +4.6 a day against a mean of 33**: inside its tolerance, but
that tolerance is the absolute floor rather than the proportional one, and it came to 14% of the
mean per day. Three days leaves two daily means, so that figure is a difference between two days
rather than a trend, and the two cannot be told apart without more days.

### Seven days, to settle that one number

168 checkpoints, six daily means after the warm-up day, and no violations anywhere:

```
 day    queues    routes    RSS MB
   1      8.96    264.83     66.30
   2      9.88    259.58     66.30
   3     11.33    263.71     66.30
   4      9.04    262.17     66.30
   5     11.08    264.58     66.30
   6     11.29    263.29     66.30
```

Neither series is monotone. The queues go up, up, *down*, up, up -- day four falls back below day
two -- and the route pool ends below where it started. The regression through them reports +0.371
and +0.164 a day, which is what a least-squares fit does to six noisy points; the resident set is
66.30 MB on every one of the six days, to two decimals.

So the answer is **no accumulation**, and the three-day figure was the difference between two days
exactly as it looked. What this really establishes is about the test rather than the city: two daily
means are not enough to call a leak, and the honest reading of a short soak is that its accumulation
section has not answered yet. The verdict reports the number of days it had, which is what lets a
reader tell those two cases apart.

### The 100 000-citizen run

A three-day soak at full scale was stopped by its own time limit 52 checkpoints in, which is enough
to answer both questions it was asked.

**Invariants: zero violations at all 52 checkpoints**, across two and a half simulated days of a
hundred thousand people, in both frame models.

**Memory: flat.** The resident set goes 94.6 -> 96.4 MB over the first eighteen checkpoints, takes
a single 32.8 MB step, and then does not move again for **thirty-three consecutive checkpoints**.
The step is the soak's own snapshot check building a second `Simulation` and stepping it for an
hour; the allocator keeps the arena afterwards. It lands inside the warm-up day and is discarded
with it -- which is now a stated requirement of where that check runs rather than a coincidence,
because anywhere later it would read as a leak in a run whose whole purpose is to find one.

It is recorded rather than fixed because it is a different subsystem, it appears only in a
configuration that is saturated by construction, and the supported city is clean: at 3.3 km the
soak drains to **20000 of 20000 citizens indoors, zero vehicles and zero route slots** every night.
The regression test asserts the precise invariant (no two buses in one place) at the oversubscribed
configuration and the outcome (nobody still aboard at three in the morning) at the ordinary one.

---

## P22 — Freezing the baseline

The point of a baseline is to be able to say, later, whether a difference came from the thing being
measured or from everything around it. So both halves are recorded: the exact commits of cna-city,
CNA and sharp-runtime, the compiler and the build flags -- and a set of figures to compare.

- [x] **P22.1 The frozen figures are world digests, not milliseconds.** A baseline of wall-clock
  numbers freezes the machine it ran on: this one measures 0.43 ms and 1.25 ms for the same work
  ten minutes apart depending on what else woke up, and a baseline that cries wolf is a baseline
  people learn to ignore. A digest has no timing in it at all and changes only when the city does,
  which makes `verify` a regression test with nothing to argue about. The timings stay in
  `--report`, where a spread is printed beside every figure and a difference smaller than the
  spread is not called a change.

- [x] **P22.2 Six digests per scenario**, so a mismatch is a lead. City means the generator moved;
  agents alone means the schedule or the steering; traffic alone the road model; transit alone the
  metro or the buses; world alone the clock or the weather.

- [x] **P22.3 Provenance beside the digests.** `environment.txt` exists so that "it changed" can be
  answered with "because of what". Every line in it is a licence for a digest to differ.

- [x] **P22.4 Two tiers.** `quick` is the generator and the oversubscribed transit network, a
  couple of minutes, and belongs in CI on every commit -- between them they cover where every
  defect found so far has actually been. `full` adds the hundred-thousand-citizen day and the
  quarter-million sweep and is the better part of an hour, because `--checksum` runs every scenario
  three times and the half-step re-run is twice the ticks on its own.

- [x] **P22.5 A scenario is checked against itself before it is frozen.** `capture` refuses to
  record one whose own re-runs -- at half the step size, and on a different worker count --
  disagree. Freezing a number this build cannot reproduce twice would be worse than freezing
  nothing.

- [x] **P22.6 The failure path is tested rather than assumed.** Corrupting a recorded digest makes
  `verify` name the scenario, name the part that moved, say what to do about it and exit 1. A
  verifier that has only ever been seen passing is the same mistake as an invariant checker that
  has only ever been seen passing, and P21 is the reason that sentence is in this file twice.

### One thing this got wrong first

`capture` wrote straight into `baseline/checksums.txt`, and the shell redirect truncates its target
the moment the block starts. The first full capture was interrupted forty minutes in, and what it
left behind was a baseline holding the four scenarios that had finished and silently missing the
fifth -- a file that still verifies, and verifies the wrong thing. It now builds the file aside and
moves it into place, so an interrupted capture leaves the previous baseline exactly as it was.

---

## P23 — The renderer matrix

The test this project was built to make possible: the same city, seed, hour, weather, camera and
population through three of CNA's renderers, with the differences in one table.

- [x] **P23.1 OPENGLES3, OPENGL33 and VULKAN**, four viewpoints each, on GL ES 3.2 / GL 4.6 core /
  Vulkan 1.4 (RADV). The two GL renderers draw the same picture: identical draw calls, identical
  triangle counts, both shadow cascades and both prepasses present, five post passes each.

- [x] **P23.2 Vulkan does not.** No shadow pass, no depth-normal prepass, no post chain, no GPU
  pass timings at all, 61 fewer draw calls, and no props, vehicles or people. Screenshots in
  `shots/renderers/` show what that looks like beside the other two.

- [x] **P23.3 Two independent causes, separated rather than lumped.** The missing instancing is
  the system working: the Vulkan backend reports `MultiStreamVertexInput` as unsupported *on
  purpose*, with a comment saying it would rather reject a draw than render it from the wrong
  stream, and that answer travels through `InstancedRendererEXT` to cna-city's HUD. The missing
  passes are one bug wearing four hats, now `CNA-FINDINGS.md` A9: the engine layer writes GLSL and
  the Vulkan backend's shader entry point takes SPIR-V, so every CNAEXT pass that owns a shader
  fails to compile and degrades to copy-through.

- [x] **P23.4 The timings from that run are not reported.** The load average recorded in each
  `system.json` was 23, 58 and 26 on a sixteen-thread machine, so the numbers describe what else
  the box was doing. What does not move with load -- draw calls, triangle counts, which passes ran,
  what the frame looks like -- is what the findings rest on. The report's load-average field
  earned its place here: without it this section would have been a table of confident nonsense
  about OPENGL33 being 1.5x slower than OPENGLES3.

### What A9 shows about CNA that is easy to miss

Nothing crashed and nothing lied. Every pass logged exactly what happened and why -- *"its shader
did not compile on the VULKAN renderer, so the pass will copy its input through instead of
running"* -- which is precisely the diagnostic A5 complains is missing everywhere but transparency.
`CascadedShadowMap` reports `isSupported() == false` so a caller can react, and cna-city does, and
the user is told on screen. The degradation is graceful and legible from top to bottom.

That is worth saying plainly, because a findings list naturally collects the failures: the layer
handled a total shader-compilation failure across every one of its passes without a single
incorrect frame or a single silent one. What is wrong is that it is happening at all.
