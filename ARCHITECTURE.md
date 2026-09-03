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
| Nobody ever went to work | A decision pass offers an eighth of the population and plans a few hundred; every other candidate had its "already commuted today" bit set anyway | 99 000 of 100 000 citizens still at home at half past eight, and staying there all day |
| Half the city stopped deciding | The decision stride cycled on the tick counter, which was correct only while decisions ran on every tick; once they moved onto simulated time the sequence became 0, 2, 4, 6 and the four odd strides were never selected | The population on foot at the peak fell from six thousand to two -- a change that looks like tuning |
| Buses carried nobody | `VehicleKind::Bus` was a body shape in the fleet mix, handed to one private commuter in twenty-five | Four hundred citizens a day driving a twelve-metre bus to work alone, past shelters that no service called at |
| The follow camera could not keep up | It eased toward its target with a 0.28 s half-life of *real* time while the world runs at sixty times real time | A subject on a 21 m/s train sat five hundred metres ahead of the lens: half a kilometre of empty tunnel with a train the size of a pixel at the end of it |

The movement integrator now sub-steps at a fixed half second regardless of the clock, junctions are
signalised only where three arterial arms meet (120 of 1 476), and a vehicle that has not moved for
four simulated minutes gives up and is counted. **That count is reported rather than hidden**: a
gridlock give-up rate that climbs is a real result about the network, and burying it would defeat
the purpose of the program.

**Public transport is two networks with one shape.** `MetroNetwork` and `BusNetwork` are separate
files that do the same four things: lay out stops, thread routes through them, run vehicles along
those routes with a trapezoidal speed profile and a dwell, and answer "can you get me from here to
there". Passengers use them through the same four-state sequence -- walk, queue, ride, walk -- with
the second walk planned on arrival rather than up front, which is what lets a leg be replanned
without a multi-leg route structure. Both refuse trips that need a change, and for the same reason:
a passenger whose plan needs a second vehicle waits for one that by construction is never going to
serve their destination, and the queue never drains. Both have a give-up timer for the same reason
again -- nothing in this simulation may wait forever.

The two differ where the city does. The metro runs on its own alignment and ignores the surface
entirely; the buses run on the road graph, are placed in the kerbside lane of whatever class of
road they are on, and stop at the same signals the cars do. They are not in the car-following
stream, which is stated as a limitation in [`plan.md`](plan.md) rather than left to be discovered.

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

Ambient occlusion is a **contract rather than a switch**, and it is the clearest example of one in
the layer: `RenderPipeline` will not produce it from a switch, because it needs a depth and a normal
image that only the game can draw. `DepthNormalPrepass` is filled with a third pass over the visible
chunks, using *its* effect and not the scene's -- calling the scene effect's `Apply` inside the
prepass replaces the program, and the "depth" recorded is then the shaded frame's red channel, which
makes SSAO compare shading against shading and produce a weak, plausible dimming everywhere instead
of occlusion at contacts. It costs 0.9 ms at street level.

The analytic sky is `AtmosphericSky`. It models a clear atmosphere and has no cloud term, and it is
undefined below the horizon -- the model keeps integrating and returns a saturated red shading to
yellow-green, which arrives as a band of colour along every roofline in a night frame. So it is not
drawn once the sun has set, and cloud cover and night are painted over it as one blended sheet whose
weights are both zero on a clear day. See [`CNA-FINDINGS.md`](CNA-FINDINGS.md) A4.

## 6. What CNA could not do here

Three boundaries were hit that are properties of the engine rather than of this program, and all
are worth stating because a demo that only reports successes is not measuring anything. A fourth
looked like one for a day -- the underground lit by an eight-a.m. sky -- and was not: CNA's cascades
do place the light far enough back for the ground to occlude the sun, and this program is the thing
that does not draw the ground into them. That is `CNA-FINDINGS.md` C4.

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

The full list, with what was checked to establish each one -- and the four things that looked like
engine defects and turned out to be this program's own mistakes -- is in
[`CNA-FINDINGS.md`](CNA-FINDINGS.md).

## 7. Measured

**Machine.** AMD Radeon 780M (radeonsi, Mesa 25.0.7), 16 threads, Debian 13, GL ES 3.2 through
CNA's `OPENGLES3` renderer. Release build, ccache, GCC 14.2.

### Simulation, no graphics device (`--bench`)

Six simulated hours from 06:30 — the morning peak, which is the only interesting part of the day —
at 30 ticks per second of wall clock and a time scale of 60.

| agents | setup | mean | p99 | worst | decide | walk | crowd | traffic | metro | bus | memory | peak travelling |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 000 | 18 ms | 0.32 ms | 0.41 ms | 2.93 ms | 0.01 | 0.02 | 0.11 | 0.14 | 0.00 | 0.04 | 7.7 MB | 98 |
| 10 000 | 22 ms | 0.64 ms | 1.05 ms | 3.66 ms | 0.06 | 0.17 | 0.13 | 0.17 | 0.00 | 0.06 | 9.3 MB | 841 |
| 50 000 | 26 ms | 1.66 ms | 2.74 ms | 6.86 ms | 0.23 | 0.50 | 0.32 | 0.27 | 0.02 | 0.12 | 16.7 MB | 4 087 |
| 100 000 | 31 ms | 2.54 ms | 4.08 ms | 8.75 ms | 0.43 | 0.62 | 0.44 | 0.40 | 0.04 | 0.19 | 25.8 MB | 8 564 |

**A hundred times the agents costs 8.0 times the tick.** That is not an accident and it is the most
interesting number the program produces: the route cache's hit rate *rises* with population -- 12%
at a thousand agents, 30% at ten thousand, 37% at fifty thousand, 39% at a hundred thousand --
because citizens do not have uniformly random destinations. They go to the same few thousand
doorways, and the more of them there are the more often somebody has already made the trip. The
cost that scales linearly is the movement of the people actually outdoors, and at the morning peak
that is 9% of the population rather than all of it.

The two transport networks are the cheapest things in the tick and stay that way: the metro is
0.04 ms and the buses 0.19 ms at a hundred thousand citizens, against 0.40 ms for the cars. Nineteen
trains and ninety-four buses is a hundred and thirteen vehicles, and the passengers on them cost
one pass over the few hundred people actually aboard. The buses cost five times the metro because
they have five times the stops and their route planner searches every stop within five hundred
metres of a trip's origin rather than the nearest station.

The worst-case tick is three to eight times the mean. It is the morning peak's burst of route
planning, which is why the planner has a per-pass budget of 320 -- a number taken from the measured
cost of a plan, about 13 microseconds averaged over cache hits and misses -- and defers the
overflow. Before that budget existed at its current size, a single decision pass on the evening peak
cost **11.6 ms**, which was the largest item in the frame and larger than the entire renderer.

### Threads

The same six simulated hours at a hundred thousand agents, varying only `--threads`:

| threads | mean tick | of which walking |
|---:|---:|---:|
| 1 | 2.61 ms | 1.02 ms |
| 2 | 2.22 ms | 0.67 ms |
| 4 | 2.10 ms | 0.50 ms |
| 8 | 2.08 ms | 0.46 ms |

**The parallel half saturates at about four threads, and the whole tick gains twenty per cent.**
That is Amdahl rather than a defect, and the two halves it separates are worth naming: route
planning, the mode lists and the crowd-grid build are serial by construction, and the parallel part
-- moving the people who are outdoors -- is memory-bound rather than compute-bound, because it
reaches into the agent arrays through a spatial hash. Eight threads waiting on the same cache
misses is not eight times the work.

### Rendering

1600 × 900, high quality, `--agents 100000`, all four shadow cascades, HDR with ACES, bloom and
FXAA.

Taken at 09:00 on a city that has been running long enough to have 41 000 people at work, 3 000 on
foot, 1 200 driving, 130 on the underground and 200 on the buses.

| view | frame | simulation | draw | shadows | prepass | scene | draws | triangles |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| city overview, 400 m up | 17.5 ms (57 fps) | 5.8 ms | 13.7 ms | 5.8 ms | — | 6.2 ms | 640 | 197 k |
| street level, morning rush | 11.5 ms (87 fps) | 6.1 ms | 7.7 ms | 1.6 ms | 0.7 ms | 1.5 ms | 210 | 45 k |

These are the best of four runs each. The machine was not otherwise idle while they were taken and
the spread between runs was two to three milliseconds, which is larger than several of the
differences this table would otherwise invite you to read into it.

The underground is skipped entirely by every pass whenever the camera is above ground -- both the
scene and all four shadow cascades. Eleven metres of earth is between the tunnels and everything
else, so they cannot appear and they shade nothing, but they run through most of the city's chunks
and were costing four extra draw calls per visible chunk, four times over in the cascades. Removing
them took three milliseconds off the overview.

Ambient occlusion switches itself off above roof height, which is why the overview has no prepass
line: it is a contact effect, and from four hundred metres up one screen pixel is several metres of
pavement. That returns 2.9 ms, about a fifth of the frame.

The environment rebuild is the one visible hitch: 11–15 ms, three or four times in a simulated day,
whenever the sun has moved far enough that the ambient would otherwise be stale. It is on the HUD
rather than hidden.

The simulation and the draw are serial here, and which of them dominates depends entirely on where
the camera is: from four hundred metres up the four shadow cascades and the visible chunk count put
the renderer ahead, and at street level the simulation is the larger of the two even though almost
nothing is on screen. **That is the answer this demo was built to get**, and it is not the one a
graphics demo would expect: the static city is 246 000 triangles in 121 chunks, which a 780M draws
without noticing, and the cost is in the hundred thousand daily routines behind it.

The frame time reported here is the whole frame, taken from the harness. Timing only the body of
`Draw` was the first version and it reported 65 fps for a frame that also spent fifteen milliseconds
in `Update` -- a program built to measure something must not be the thing that is measured wrongly.

Static geometry is 25.3 MB of vertex and index buffers; the procedural textures are 11.5 MB
including their mip chains; the simulation is 24.5 MB. The whole city is under 65 MB.

## 8. Determinism

Everything generated — the network, the buildings, the population, every agent's home, workplace
and schedule — is a pure function of one 64-bit seed through a PCG32 stream, and every subsystem
draws from its own sub-stream so that adding a draw in one of them does not move every number in
the others. Two runs at different agent counts are the same city.
