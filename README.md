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
| **Traffic** | Lane-based vehicles using the Intelligent Driver Model for car-following, with junction priority and signalised intersections. |
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

## Camera modes

| Key | Mode |
|---|---|
| `1` | Free camera — fly anywhere. |
| `2` | Orbit — circle the downtown skyline. |
| `3` | **Follow a citizen** — pick one of the hundred thousand and watch their entire day: waking, walking to the metro, riding to work, lunch, the commute home. `--follow-metro` and `--follow-bus` start it on somebody who is actually on public transport, which a random pick of a hundred thousand people almost never is. |
| `4` | Street level — a fixed camera on a pavement corner, watching the city go past. |
| `5` | Cinematic — a slow scripted sweep, for capture. |

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
