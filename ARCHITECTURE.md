# CNA City — architecture and findings

This document is written from the code, and every number in it was measured on the machine named
in §7 rather than estimated. Where a decision turned out to be wrong, the wrong version is recorded
next to the right one: the mistakes are the part of a technology demonstration that is worth
keeping.

---

## 1. The one-sentence version

A seed produces a road network; the network's bounded faces are the city blocks; the blocks produce
buildings; the buildings produce homes and workplaces; those produce a hundred thousand daily
schedules; the schedules produce the traffic — and the renderer only ever sees the result as static
chunks and instance buffers.

## 2. Layering

```
        Program.cpp / CliOptions            argv, run mode
                 |
      +----------+-----------+
      |                      |
  CityGame                Benchmark          window, camera, HUD  |  no device at all
      |                      |
      +----------+-----------+
                 |
             Simulation                     the fixed tick
      /      |        |        \
   City   Metro   Pathfinder  Traffic
      \      |        |        /
            Agents (SoA) + RoutePool
```

Nothing below `CityGame` knows that a graphics device exists, and nothing in the renderers writes
simulation state. That is what makes `--bench` and `--headless` able to run the entire city with no
window, and therefore what makes the benchmark measure the city rather than the driver.

## 3. Generation

**The road network is a planar graph, not a grid.** Polylines are dropped onto the plane — an
arterial grid on jittered spacing, a boulevard ring, three diagonal avenues, and a rotated street
grid per district — and then cut against each other, welded, and pruned of dead ends. City blocks
are the graph's *bounded faces*, found by the standard half-edge walk, which is why a diagonal
avenue produces the triangular plots and five- and six-way junctions a real city has and a
generated grid does not.

Two generation defects are recorded in the source and both were invisible until the totals were
checked against arithmetic:

- District streets first stopped 0.6 m short of the arterials they were meant to meet. Each
  district's grid became an island, the face between it and the surrounding arterials was an
  annulus rather than a polygon, and the extractor returned a few hundred enormous self-touching
  "blocks" instead of a thousand real ones. The tell was that the total face area came to 10.8
  million m² for a city of 9.6 million.
- The footprint test rejected three quarters of every street wall it tried to build, because a
  building against a frontage has its two front corners exactly *on* the block boundary, where a
  point-in-polygon test answers whichever way the arithmetic falls.

The generator produces, from one seed, in **24 ms**: 1 476 junctions, 2 547 road segments, 204 km
of road, 1 072 blocks, 11 808 buildings holding 134 000 residential places and 175 000 jobs, and
19 900 props.

## 4. Simulation

**Struct-of-arrays, and it is not decoration.** The tick touches `position`, `heading` and `speed`
for the agents that are moving and nothing else. An array-of-structs would pull ninety bytes of
schedule and route state through the cache for each of them, and the headline number would be ten
thousand rather than a hundred thousand.

**Route planning is two-level with a direct-mapped cache.** A district-graph search produces a
corridor; the road-level A\* is restricted to it plus one ring. The city's road graph is small
enough that a flat A\* would be fast for *one* query — the problem was never the size of a search,
it was that there are two hundred thousand of them in a simulated day.

**The car-following model is Treiber's IDM**, unchanged from its published form, because it is the
cheapest model that produces the two things a city demo lives on: a queue that forms behind a red
light and discharges in a wave, and stop-and-go waves on a busy road with no obstacle at all.

Four simulation defects are recorded in the source:

| What broke | Why | What it looked like |
|---|---|---|
| Pedestrians never arrived | At a time scale of 180 a frame is six simulated seconds and a walker covers eight metres, so a waypoint with a three-metre arrival radius is never reached | 13 000 citizens oscillating across their last junction forever at a healthy 1.3 m/s, and a walking population that never went down |
| Metro passengers waited forever | Routes were planned with an interchange, but a passenger can only board a train that serves their destination | 443 people still standing on platforms at three in the morning |
| Total gridlock | Every car was spawned at the head of its first segment, so two drivers leaving the same street seconds apart spawned *inside* each other; the IDM answers a negative gap with maximum braking, for ever | 4 000 vehicles at a mean speed of 0.2 m/s that read as congestion and was not |
| Total gridlock, again | Signalising every junction whose highest class was a collector put lights on nine junctions in fourteen; queues on the short segments grew back past their own entrances | The fleet stopped draining between peaks |

The movement integrator now sub-steps at a fixed half second regardless of the clock, junctions are
signalised only where three arterial arms meet (120 of 1 476), and a vehicle that has not moved for
four simulated minutes gives up and is counted. **That count is reported rather than hidden**: a
gridlock give-up rate that climbs is a real result about the network, and burying it would defeat
the purpose of the program.

## 5. Rendering

**Buildings are baked, props are instanced, and the split is by identity rather than by count.**
Twelve thousand buildings is a quarter of a million vertices — nothing — and baking them into
per-chunk buffers buys the one thing instancing cannot: every building's UVs come from its own
dimensions, so a window is 3.2 m wide on a bungalow and 3.2 m wide on a 180 m tower. Instancing
forces one UV scale across every instance, and the stretched facades that produces are the classic
tell of a generated city. A lamp post, by contrast, is the same lamp post twenty thousand times
over, so it goes through `InstancedRendererEXT`.

**There is not one image file in the project.** Facades, road markings with per-class lane
geometry, pavement slabs, bark, foliage and roofs are all painted at start-up and uploaded with
box-filtered mip chains built in linear light. A city is almost entirely minified — at four hundred
metres up nearly every texel is below one pixel — and undefined lower mip levels or sRGB-space
averaging both show immediately.

Four rendering defects are recorded in the source:

- **Face winding.** Under this renderer's default rasterizer state a triangle is drawn when its
  right-hand-rule winding normal points *away* from the camera. The ground ribbons were wound the
  other way, so every carriageway in the city was back-facing and invisible; what showed through
  was the ground plane, giving a strip of grass down the middle of every street with the lamps and
  street trees correctly placed on either side of it.
- **Coplanar surfaces.** Filling each block out to the road centrelines and drawing the carriageway
  on top of it works for about eighty metres and then loses to depth precision at a grazing angle.
  Footways are now generated per block edge from *that edge's* kerb line, so the two never overlap.
- **Ambient.** `PbrEffect`'s ambient term is an irradiance that reaches the surface without the
  albedo division intuition assumes — roughly an order of magnitude stronger than it reads. Setting
  it to zero and re-rendering is what showed it: the night city went from a bright sepia photograph
  with invisible windows to darkness with the windows glowing.
- **Exposure.** It was raised after dark on the reasoning that night is dark. It is not: at night
  the *emitters* dominate, and lifting exposure blew the city to white.

The analytic sky is `AtmosphericSky`, which models a clear atmosphere and keeps producing sunset
red below the horizon. Rather than replace it, cloud cover and night are painted over it as one
blended sheet whose weights are both zero on a clear day.

## 6. What CNA could not do here

Two boundaries were hit that are properties of the engine rather than of this program, and both are
worth stating because a demo that only reports successes is not measuring anything.

**Thousands of street lights at night are not real lights.** `ClusteredForwardEffect` is the layer's
own PBR effect and it is what supports many punctual lights; `PbrEffect` is what supports the
texture set — albedo, normal, metallic-roughness, emissive, occlusion. They are different effects
and a surface gets one of them. A city needs the textures far more than it needs the lights, so the
lamps here are emissive geometry plus bloom, and the pools of light they should cast on the road do
not exist. Making both available to one surface is a change inside CNA, not one a game can make.

**There is no attribute slot for a per-instance colour.** CNA's instance stream occupies vertex
attribute locations 12 to 15 and the stock lit shaders use 0 to 11 — sixteen in total, which is
XNA's own ceiling and GL ES 3's guaranteed minimum. A crowd is therefore bucketed into eight
clothing colours and drawn once per bucket, and vehicles into eight paints. It costs a few dozen
draw calls and it is the only way to have a crowd that is not uniformly grey without writing a
custom `ShaderEffect` and giving up the stock lighting.

**sharp-runtime's `Parallel::For` calls `std::async(std::launch::async, ...)` per iteration**, so
every iteration is a fresh operating-system thread. That is the right shape for population
generation, which runs once over a hundred thousand agents and is where this program uses it. It is
the wrong shape for five loops that run thirty times a second, so the tick uses a persistent pool
instead.

## 7. Measured

**Machine.** AMD Radeon 780M (radeonsi, Mesa 25.0.7), 16 threads, Debian 13, GL ES 3.2 through
CNA's `OPENGLES3` renderer. Release build, ccache, GCC 14.2.

### Simulation, no graphics device (`--bench`)

Six simulated hours from 06:30 — the morning peak, which is the only interesting part of the day —
at 30 ticks per second of wall clock and a time scale of 60.

| agents | setup | mean | p99 | worst | decide | walk | crowd | traffic | memory | peak travelling |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 000 | 19 ms | 0.29 ms | 0.41 ms | 2.97 ms | 0.01 | 0.01 | 0.12 | 0.15 | 7.5 MB | 80 |
| 10 000 | 19 ms | 0.54 ms | 0.85 ms | 8.62 ms | 0.06 | 0.09 | 0.13 | 0.18 | 9.0 MB | 715 |
| 50 000 | 21 ms | 1.59 ms | 2.66 ms | 9.73 ms | 0.19 | 0.43 | 0.26 | 0.27 | 15.9 MB | 3 882 |
| 100 000 | 24 ms | 2.57 ms | 4.33 ms | 16.26 ms | 0.27 | 0.59 | 0.35 | 0.44 | 24.5 MB | 9 230 |

**A hundred times the agents costs 8.9 times the tick.** That is not an accident and it is the most
interesting number the program produces: the route cache's hit rate *rises* with population — 8% at
a thousand agents, 23% at ten thousand, 32% at fifty thousand, 38% at a hundred thousand — because
citizens do not have uniformly random destinations. They go to the same few thousand doorways, and
the more of them there are the more often somebody has already made the trip. The cost that scales
linearly is the movement of the people actually outdoors, and at the morning peak that is 9% of the
population rather than all of it.

The worst-case tick is four to six times the mean in every run. It is the morning peak's burst of
route planning, which is why the planner has a per-tick budget and defers the overflow.

### Rendering

1600 × 900, high quality, `--agents 100000`, all four shadow cascades, HDR with ACES, bloom and
FXAA.

| view | frame | simulation | shadows | scene | instanced | draws | triangles |
|---|---:|---:|---:|---:|---:|---:|---:|
| city overview, 400 m up | 15.3 ms (65 fps) | 15.4 ms | 6.7 ms | 4.7 ms | 1.5 ms | 442 | 173 k |
| street level | 12.4 ms (80 fps) | 4.1 ms | 1.7 ms | 1.8 ms | 1.5 ms | 210 | 73 k |
| chase camera on one citizen | 11.7 ms (86 fps) | 2.1 ms | 1.0 ms | 0.9 ms | 1.0 ms | 189 | 58 k |

The simulation and the frame overlap in wall-clock terms only in the sense that both happen; they
are serial here, and at a hundred thousand agents from an overview the simulation is the larger of
the two. **That is the bottleneck this demo was built to find**, and it is not in the renderer: the
static city is 220 000 triangles in 121 chunks, which a 780M draws without noticing.

Static geometry is 25.3 MB of vertex and index buffers; the procedural textures are 11.5 MB
including their mip chains; the simulation is 24.5 MB. The whole city is under 65 MB.

## 8. Determinism

Everything generated — the network, the buildings, the population, every agent's home, workplace
and schedule — is a pure function of one 64-bit seed through a PCG32 stream, and every subsystem
draws from its own sub-stream so that adding a draw in one of them does not move every number in
the others. Two runs at different agent counts are the same city.
