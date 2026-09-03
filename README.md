# CNA City

**A procedural city with 100 000 simulated inhabitants, built on [CNA](../cnanext) and
[sharp-runtime](../sharp-runtimenext).**

CNA City is not a game. It is a technology demonstration whose only purpose is to push the CNA
runtime — the C++ reimplementation of the XNA 4.0 programming model — until something in it bends,
and to say precisely what bent. A city is the workload chosen for that, because a city is the one
scene that is simultaneously hostile to every subsystem at once: a hundred thousand agents to
simulate, tens of thousands of instanced objects to cull and draw, fourteen thousand street lamps
and a hundred thousand lit windows after dark, a sky that changes for twenty-four simulated hours,
and a camera that can drop from an overview of the whole map to the shoulder of one pedestrian
without a loading screen.

The demo deliberately uses **more than XNA 4.0 ever had**: cascaded shadow maps, an analytic
atmospheric sky that also supplies the ambient through image-based lighting, HDR with ACES
tonemapping, SSAO fed by a depth-normal prepass, bloom, height fog, GPU timer queries, hardware
instancing and compute-free procedural texturing. Three things in the `CNAEXT` layer are
deliberately *not* switched on — clustered forward lighting, volumetric fog and depth of field —
and the reason for each is in [`CNA-FINDINGS.md`](CNA-FINDINGS.md) rather than left unsaid, because
"what can CNA actually do" is the question this program exists to answer and half of an honest
answer is what it cannot.

---

## What it simulates

| System | What it does |
|---|---|
| **City generation** | Deterministic, seeded. Arterial grid → secondary streets → blocks → lots. Districts are zoned (downtown, commercial, residential, industrial, park) and the zoning drives building height, footprint and material. |
| **Population** | 100 000 agents in a struct-of-arrays store. Each has a home, a workplace, and a 24-hour schedule; the day is simulated, not scripted. |
| **Pathfinding** | Three-level hierarchy: a district graph, a road-node A\* inside districts, and local steering. A shared path cache means a hundred thousand agents do not plan a hundred thousand paths. |
| **Traffic** | Lane-based vehicles using the Intelligent Driver Model for car-following, with junction priority and signalised intersections. Buses share the same lane occupancy, so a bus at a stop is something the traffic behind it queues for. |
| **Pedestrians** | Sidewalk lanes plus a spatial-hash crowd separation step, so a crossing at rush hour actually queues. |
| **Traffic lights** | Phase-cycled intersections; vehicles, buses and pedestrians all obey them. |
| **Metro** | Underground lines with stations, timetabled trains, and agents that board, ride and alight as part of a commute. The tunnels, platforms, rails, lit strips and lit carriages are all there, and the follow camera can ride with a passenger. |
| **Buses** | Fourteen routes of stops on the arterials and collectors, threaded through the road graph, with a fleet that dwells at every stop and citizens who queue at the shelter, board, ride and get off. |
| **Day and night** | A 24-hour clock drives the sun, the sky's turbidity, the street lights, and the lit windows. |
| **Weather** | Clear, overcast, rain, fog and snow, each changing the sky, the fog, the particle layer and the wetness of the road surface. |

## What it renders

Everything is generated at start-up — there is not one image file in the project — and drawn
through the `CNAEXT` engine layer:

- `AtmosphericSky` for a physically-derived sky that changes across the day, and
  `EnvironmentProcessor` turning the *same model* into the irradiance and prefiltered specular that
  `PbrEffect::setImageBasedLightEXT` lights the city with. The ambient is the sky, not a constant.
- `CascadedShadowMap`, four cascades, for sun shadows across a city-scale view.
- `RenderPipeline` with HDR and ACES, bloom, SSAO fed by a `DepthNormalPrepass` the game draws
  itself, height fog, light shafts and FXAA. Chromatic aberration and film grain at ultra.
- `InstancedRendererEXT` and `LodGroupEXT` for the tens of thousands of *identical* lamps, trees,
  vehicles and people; buildings are baked into per-chunk buffers instead, so each one's facade UVs
  come from its own dimensions. `FrustumCullerEXT` culls both.
- `DebugDraw` for the road-network and route overlays, and `RenderPipeline::setGpuTimingEnabledEXT`
  with `getPassTimingsEXT` (`F3`) so a demo that claims to find bottlenecks can name them.

Three things it deliberately does **not** use, and why — all three are in
[`CNA-FINDINGS.md`](CNA-FINDINGS.md):

- **`ClusteredForwardEffect`.** It is what carries many punctual lights, and it cannot carry a
  texture set at the same time. A city needs the textures more, so the fourteen thousand street
  lamps here are emissive geometry and bloom, and they cast no light on the road. That is the
  single largest thing this demo cannot show, and it is an engine boundary rather than a choice.
- **`ParticleSystem`.** Rain and snow are this project's own instanced geometry instead. The
  engine's particle system falls back to a CPU simulation without compute, but it also cannot be
  told to follow the camera the way a precipitation column has to, and a fixed budget of streaks
  wrapped around the viewer is both cheaper and what the effect actually needs.
- **Volumetric fog.** `RenderPipeline` adds the pass whenever the density is above zero and never
  gives it a light, nor exposes it so a game could. With no light and no shadow map it integrates
  scattering against the empty sky and returns a band of red and green above every roofline. It is
  a switch that turns on an artefact (A8).

## Overlapping the simulation with the draw

```sh
./build/cna-city --frame-model pipelined     # serial is the default
```

The simulation and the draw run one after the other. `--frame-model pipelined` starts the step
after the instances have been collected — which is the last thing in the frame that reads the
simulation — and joins it before the overlay and the HUD, which read it again. No snapshot: the
ordering makes one unnecessary, and a snapshot of a hundred thousand citizens would cost more than
it saved.

It reports the **blocked** time: how much of the step the draw failed to cover. That is the number
that answers the question, and it is far less sensitive to what else the machine is doing than a
frame time is. On a quiet machine it is **0.01–0.3 ms against a step of 3–5 ms** — the draw covers
essentially the whole simulation. Under load it rises to 8–10 ms, because the simulation already
uses every core through its own pool and the two halves then compete rather than overlap.

What that is worth in frames per second, this machine could not say: three runs of the *identical*
serial configuration measured 13.0, 37.7 and 40.1 ms for the same viewpoint, which is three times
the difference being looked for. The experiment is built and instrumented; the measurement wants a
quiet machine. See [`plan.md`](plan.md) P19.

## Camera modes

| Key | Mode |
|---|---|
| `1` | Free camera — fly anywhere. |
| `2` | Orbit — circle the downtown skyline. |
| `3` | **Follow a citizen** — pick one of the hundred thousand and watch their entire day: waking, walking to the metro, riding to work, lunch, the commute home. `--follow-metro` and `--follow-bus` start it on somebody who is actually on public transport, which a random pick of a hundred thousand people almost never is. |
| `4` | Street level — a fixed camera on a pavement corner, watching the city go past. |
| `5` | Cinematic — a slow scripted sweep, for capture. |

## Heatmaps

`F4` cycles a layer that paints a *cost* over the city, on its own key rather than folded into
`Tab`: an overlay shows what the simulation is, a heatmap shows what it is costing, and the two are
worth looking at together. Also `--heatmap traffic|density|render|paths`, because a screenshot has
no keyboard.

| layer | what is red |
|---|---|
| **traffic** | Mean speed per road segment against *that road's own limit* — an arterial at 8 m/s is flowing and an alley at 8 m/s is not, so colouring by absolute speed paints the whole suburb red and says nothing. |
| **density** | People and vehicles per 100 m cell. The cell size is what makes it readable: finer is speckle, coarser smears a queue at one junction across a district. |
| **render** | Triangles per *visible* chunk. A culled chunk is not costing anything, so it is not drawn. |
| **paths** | Route searches per district, on a ten-second half-life. It counts cache **misses**, because a hit costs nothing — and it decays, because a cumulative count is uniform by lunchtime and answers no question anybody has. |

Every one of them puts its scale on the HUD. A heatmap without a legend is a picture of where the
red is, and the same city looks alarming or fine depending on what the brightest cell happened to
be.

`Tab` cycles the overlay — off, statistics, the road graph the planner actually sees, and the live
routes of the citizens near the camera (also `--overlay roads`). `N` picks another citizen to
follow and **`L` locks the one you have**, so the camera waits outside their door instead of moving
on when they go inside — which is what makes following a whole day possible. `F` cycles the weather
and pins it, `T`/`G` wind the clock, `[`/`]` change the time scale, `P` pauses, `F1` hides the HUD,
`F2` bypasses the whole post-processing chain so you can see what it is doing (also `--no-post`,
which is the only way to capture the raw scene on a machine with nobody in front of it), and `F3`
turns on GPU timer queries and breaks the post chain down pass by pass.

## Tests

```sh
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

Seventy-one cases across twelve suites, weighted towards regression rather than coverage. That is
deliberate: this program is a benchmark, and **a simulation that quietly stops moving anybody gets
faster.** Every defect recorded in [`ARCHITECTURE.md`](ARCHITECTURE.md) did exactly that, and three
of them looked like tuning. The tests are named for the defect rather than for the function —
`PedestriansActuallyArriveAtALargeTimeScale`, `TheDecisionStrideReachesEveryAgent`,
`NoPrivateDriverIsGivenABus` — and on their first run they found five more, including every pitched
roof in the city being wound inside out and the same seed producing a different city at a different
frame rate.

GoogleTest comes from CNA's own vendored checkout, so there is nothing to fetch. Turn the suite off
with `-DCNA_CITY_BUILD_TESTS=OFF`.

### The determinism check, which does not fit in a unit test

```sh
./build/cna-city --seed 42 --agents 100000 --simulate 24h --checksum
```

```
CITY      bb4d00afc47a677b
AGENTS    c4cf0009fc568ade
TRAFFIC   8d0c002daccd37b5
TRANSIT   ac3af86cc52dcda6
WORLD     22afa8bce65d2721
FINAL     a4f1dbeb74bee2b8

reproduced
  at half the step size            yes
  on 1 worker threads instead of 16  yes
```

Five digests rather than one, because a mismatch should be a lead and not just a verdict: a city
that differs means the *generator* moved, agents alone means the schedule or the steering did,
traffic alone means the road model did. It then re-runs the whole thing at half the step size and
on a different number of worker threads and says whether those agreed — a determinism claim that is
only ever checked one way is one that has already been broken here twice.

This belongs in CI rather than in the unit suite because it takes minutes, and because it catches
things a fast test cannot. The last defect it found needed a hundred thousand citizens and eight
simulated hours: the list of arrivals is gathered in parallel with an atomic increment, an arrival
joins the *back* of a platform queue, and a train drains that queue from the *front* — so which of
two citizens got on a full train depended on which worker thread finished first. Comparing two runs
at fifty thousand agents caught that three times in six; at full scale it is reliable.

## Recording and replaying

```sh
./build/cna-city --agents 100000 --record monday.cna-replay
./build/cna-city --replay monday.cna-replay
```

A replay is about a kilobyte for a simulated day of a hundred thousand people, because it stores
the *input* and not the world: the seed, the configuration, how many fixed ticks ran, and the few
moments somebody changed the weather or wound the clock. Everything else is recomputed, which is
only possible because the tick is fixed-length and the simulation is a function of its seed.

Replaying is a test rather than a video. It compares a digest at checkpoints as it goes and stops
at the first disagreement, naming the tick and which half of the world stopped matching:

```
DIVERGED at tick 50004, in the agents
            expected         actual
  CITY      bb4d00afc47a677b bb4d00afc47a677b
  AGENTS    2364b0d2bb458145 8128adbcd2e5ab4f <--
```

`--replay FILE --threads 1` re-runs a recording on a different number of workers, which is how the
arrival-queue defect above was pinned to a tick.

## Snapshots and scenarios

```sh
./build/cna-city --headless --time 5.0 --simulate 2h --save morning.snapshot --note "07:00"
./build/cna-city --bench --load morning.snapshot          # measure the peak, no warm-up
./build/cna-city --load morning.snapshot                  # or just look at it
```

Measuring the morning peak means simulating up to the morning peak first, and at a hundred thousand
citizens that is **25 seconds** of warm-up before the first number — paid again for every scale,
every renderer and every run. Loading the same moment takes **0.27 s**.

A snapshot stores what cannot be recomputed and nothing else: the population, the traffic, the two
fleets, the clock, the weather, every random generator's state and the queues on the platforms. The
streets, the buildings, the metro lines and the bus routes are a pure function of the seed, so what
the file keeps of them is a *digest* — and a snapshot taken against a generator that has since
changed is refused rather than loaded into a world whose roads have moved under its traffic.

`scripts/make-scenarios.sh` builds the set:

| scenario | what it is |
|---|---|
| `empty-night` | 02:00, almost nobody out |
| `morning-rush` | 07:00, the peak building |
| `evening-rush` | 17:30 |
| `rain-gridlock` | 09:00 in the rain, with the roads at their worst |
| `metro-peak` | 08:00 with car ownership at 15%, so the underground carries the city |

They are 20 MB each and are not in git: they are regenerable, and a git history is not the place for
what can be recomputed.

## Build

```sh
cmake -S . -B build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
      -DCMAKE_C_COMPILER_LAUNCHER=ccache
cmake --build build -j$(nproc)
./build/cna-city
```

CNA City expects `../cnanext` and `../sharp-runtimenext` as sibling checkouts — the same layout
`cna-samples`, `cna-killer` and `cna-rts` use. It turns `CNA_CNAEXT=ON` on for itself; the layer is
off by default in CNA and this demo is the reason to switch it on.

## Run

```sh
./build/cna-city --agents 100000            # the headline number
./build/cna-city --agents 25000 --quality medium
./build/cna-city --follow                   # start in "a day in the life"
./build/cna-city --time 21.5 --weather rain # night, wet streets
./build/cna-city --follow-metro             # ride the underground with a commuter
./build/cna-city --follow-bus               # the same, on the buses
./build/cna-city --no-post                  # the raw scene, no tonemapper
./build/cna-city --bench --scales 1000,10000,100000 --csv bench.csv
./build/cna-city --checksum --simulate 24h        # a digest CI can compare
./build/cna-city --record monday.cna-replay       # keep this session
./build/cna-city --replay monday.cna-replay       # and check it still reproduces
```

`--help` lists everything.

## Benchmark reports

```sh
./build/cna-city --report results --scales 1000,10000,50000,100000 --repeat 4
```

Leaves a directory behind rather than a number in a terminal:

```
results/
    system.json      renderer, compiler, threads, seed, city digest, load average
    simulation.csv   the scaling table
    memory.csv       resident bytes per citizen
    rendering.csv    frame cost by viewpoint
    passes.csv       GPU pass timings
    report.html      all of it, with charts
```

The page is self-contained — the charts are inline SVG, so it still works when it is opened in two
years on a machine with no network.

The rendering half drives the camera through a fixed tour rather than asking somebody to stand in
the right place: a city overview where the four shadow cascades dominate, the downtown skyline, a
street at eye level where the simulation is the larger half, and a signalised junction. Each is
warmed up for as many frames as it is then measured for, so a shader compile does not become the
measurement. `passes.csv` comes from CNA's own timer queries rather than from a stopwatch around a
draw call — and it shows things a stopwatch cannot, like SSAO appearing only in the two street-level
views because ambient occlusion switches itself off above roof height:

```
view                   frame_ms  draw_ms  shadow_ms  prepass_ms  draw_calls  triangles
city overview             17.87    15.17       6.06        0.00         487     178090
street level              14.29    10.48       2.55        1.27         226      68326
```

`--headless --report` writes the simulation half only.

### Comparing two runs

```sh
./build/cna-city --compare results/before results/after
```

Writes one page with the two side by side. A difference smaller than the spread of either run is
labelled **within noise** rather than reported as a change — a benchmark that calls every wobble a
regression is a benchmark whose regressions get ignored. Two reports whose city digests differ are
refused loudly, because they are two different workloads and putting their numbers in one table is
the most confident way to reach a wrong conclusion.

`scripts/compare-renderers.sh` does the same across CNA's renderers — identical seed, hour,
weather, camera and population through OPENGLES3, OPENGL33, Vulkan and the rest. It is not run by
default and it is not cheap: each renderer is a separate configure and a full rebuild of CNA, so
the script reuses one shared scratch build directory and measures them one after another rather
than leaving a tree per renderer behind.

Each scale is measured `--repeat` times and the **fastest** run is reported with the **spread**
beside it. That is not cherry-picking: two runs of the same build do identical work, so the
difference between them is whatever else the machine was doing, and the minimum is the closest
estimate of what this program costs. The spread is the part that matters — it says whether the
number can be trusted at all. From two runs of this repository an hour apart:

| agents | mean | spread |
|---:|---:|---:|
| 1 000 | 0.46 ms | ±0.02 |
| 10 000 | 0.83 ms | ±0.08 |
| 50 000 | 1.99 ms | ±0.14 |
| 100 000 | 3.19 ms | **±0.51** |
| 100 000, machine busy | 4.79 ms | **±3.73** |

The last row is the same build on the same machine with something else running. A report that
printed `4.79 ms` as fact would be indistinguishable from a 50% regression, and the page flags a
spread that wide in the table rather than leaving it to be noticed.

## What it measures

Every one of CNA's own examples isolates one subsystem so that a failure names itself. That is the
right way to test a runtime and the wrong way to find out what it costs. This program does the
opposite on purpose: it turns everything on at once, at a scale nothing else in the workspace
reaches, and reports where the frame goes.

On an AMD Radeon 780M with 16 threads, at a hundred thousand citizens:

| | |
|---|---|
| City generation, from one seed | **24 ms** — 204 km of road, 1 072 blocks, 11 808 buildings |
| Simulation tick, mean / p99 | **2.84 ms / 4.84 ms** |
| Of which the metro and the buses | **0.04 ms and 0.24 ms** |
| Frame, street level, 1600x900 high | **11.5 ms (87 fps)** |
| Frame, city overview, all four shadow cascades | **17.5 ms (57 fps)** |
| Total resident memory | **under 65 MB** |

### A million

```sh
./build/cna-city --bench --agents 1000000 --scales 1000,10000,100000,250000,500000,1000000
```

| agents | mean tick | memory | outdoors at the peak | trips deferred | route pool exhausted |
|---:|---:|---:|---:|---:|---:|
| 100 000 | 3.9 ms | 26 MB | 8 913 | 597 | 0 |
| 1 000 000 | 47.9 ms | 191 MB | 104 592 | 5 855 | 0 |

**A thousand times the agents costs a hundred and three times the tick.** Nothing fails: no
allocation throws, the route pool never runs out, the crowd grid never degrades, and memory is
linear. At a million the simulation alone is 48 ms, so the program stops being real-time — which
is a useful thing for a benchmark to be able to say exactly rather than approximately.

What *did* stop scaling was a policy rather than a structure: the planner's per-pass budget was a
flat 320 trips whatever the population, so the share of peak demand it served fell from 35% at a
hundred thousand to 3.5% at a million. It is per hundred thousand citizens now, and the share is
35% at both ends. Finding that is what the `deferred` and `poolfull` columns are for — a tick that
stays cheap because the planner is refusing work has stopped measuring the same thing.

The other limit is the city, and it is not a defect: the default 3.3 km city has homes for 133 911
people. A million live in it seven and a half to a dwelling, quite happily. `--size 4500` gives a
9 km city that houses them properly, where the tick is 77 ms because the trips are longer — 185 918
people in transit at the peak instead of 104 592.

**A hundred times the agents costs 6.6 times the tick, not a hundred.** The route cache's hit rate
*rises* with population — 11% at a thousand agents, 37% at a hundred thousand — because citizens do
not have random destinations, they go to the same few thousand doorways.

And which half of the program dominates depends on where the camera is: from four hundred metres up
the shadow cascades put the renderer ahead, and at street level the simulation is the larger of the
two even with almost nothing on screen. The whole static city is 246 000 triangles.

What CNA could not do here is written down as plainly as what it could, in
[`CNA-FINDINGS.md`](CNA-FINDINGS.md) — seven capability gaps, each checked against CNA's own
source before it was written down, and four things that looked like engine defects and turned
out to be this program's mistakes. Every defect found on the way — including the four that
produced a city where nobody ever arrived anywhere, the one that made every road invisible, and
the one that quietly removed every roof — is in [`ARCHITECTURE.md`](ARCHITECTURE.md), with the
measurement beside each claim.

## License

MIT — see [`LICENSE`](LICENSE).
