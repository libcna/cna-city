# CNA City

**A procedural city with 100 000 simulated inhabitants, built on [CNA](../cnanext) and
[sharp-runtime](../sharp-runtimenext).**

CNA City is not a game. It is a technology demonstration whose only purpose is to push the CNA
runtime — the C++ reimplementation of the XNA 4.0 programming model — until something in it bends,
and to say precisely what bent. A city is the workload chosen for that, because a city is the one
scene that is simultaneously hostile to every subsystem at once: a hundred thousand agents to
simulate, tens of thousands of instanced objects to cull and draw, thousands of dynamic lights at
night, a sky that changes for twenty-four simulated hours, and a camera that can drop from an
overview of the whole map to the shoulder of one pedestrian without a loading screen.

The demo deliberately uses **more than XNA 4.0 ever had**. Everything in the `CNAEXT` engine layer
that makes a modern frame — clustered forward lighting, cascaded shadow maps, an analytic
atmospheric sky, HDR with ACES tonemapping, SSAO, bloom, volumetric fog, depth of field, GPU
timers — is switched on here, because "what can CNA actually do" is the question this program
exists to answer.

---

## What it simulates

| System | What it does |
|---|---|
| **City generation** | Deterministic, seeded. Arterial grid → secondary streets → blocks → lots. Districts are zoned (downtown, commercial, residential, industrial, park) and the zoning drives building height, footprint and material. |
| **Population** | 100 000 agents in a struct-of-arrays store. Each has a home, a workplace, and a 24-hour schedule; the day is simulated, not scripted. |
| **Pathfinding** | Three-level hierarchy: a district graph, a road-node A\* inside districts, and local steering. A shared path cache means a hundred thousand agents do not plan a hundred thousand paths. |
| **Traffic** | Lane-based vehicles using the Intelligent Driver Model for car-following, with junction priority and signalised intersections. |
| **Pedestrians** | Sidewalk lanes plus a spatial-hash crowd separation step, so a crossing at rush hour actually queues. |
| **Traffic lights** | Phase-cycled intersections; vehicles and pedestrians both obey them. |
| **Metro** | Underground lines with stations, timetabled trains, and agents that board, ride and alight as part of a commute. |
| **Day and night** | A 24-hour clock drives the sun, the sky's turbidity, the street lights, and the lit windows. |
| **Weather** | Clear, overcast, rain, fog and snow, each changing the sky, the fog, the particle layer and the wetness of the road surface. |

## What it renders

Instanced everything, culled and LOD-selected on the CPU, drawn through the `CNAEXT` pipeline:

- `AtmosphericSky` for a physically-derived sky that changes across the day.
- `CascadedShadowMap` for sun shadows across a city-scale view.
- `ClusteredForwardEffect` + `ClusteredLightBuffer` so that thousands of street lamps, headlights
  and windows are real lights at night rather than a painted texture.
- `RenderPipeline` with HDR, bloom, SSAO, height and volumetric fog, light shafts, depth of field,
  motion blur, colour grading, ACES tonemapping and FXAA.
- `InstancedRendererEXT`, `FrustumCullerEXT` and `LodGroupEXT` for the buildings, vehicles,
  pedestrians and props.
- `ParticleSystem` for rain and snow, `DebugDraw` for the network overlays.
- `GpuTimer` and the pipeline's own per-pass timings, because a demo that claims to find
  bottlenecks has to be able to name them.

## Camera modes

| Key | Mode |
|---|---|
| `1` | Free camera — fly anywhere. |
| `2` | Orbit — circle the downtown skyline. |
| `3` | **Follow a citizen** — pick one of the hundred thousand and watch their entire day: waking, walking to the metro, riding to work, lunch, the commute home. |
| `4` | Street level — a fixed camera on a pavement corner, watching the city go past. |
| `5` | Cinematic — a slow scripted sweep, for capture. |

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
./build/cna-city --bench --scales 1000,10000,100000 --csv bench.csv
```

`--help` lists everything.

## Why this exists

Every one of CNA's own examples isolates one subsystem so that a failure names itself. That is the
right way to test a runtime and the wrong way to find out what it costs. This program does the
opposite on purpose: it turns everything on at once, at a scale nothing else in the workspace
reaches, and reports where the frame goes. What it finds is written down in
[`ARCHITECTURE.md`](ARCHITECTURE.md) and in the benchmark section of that document, with the
measurement beside every claim.

## License

MIT — see [`LICENSE`](LICENSE).
