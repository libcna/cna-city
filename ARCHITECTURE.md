# CNA City — architecture

> This document is written as the code lands. Sections marked *(planned)* describe a decision that
> has been made but not yet implemented; every other section describes code that exists.

## 1. The one-sentence version

A deterministic seed produces a road network; the road network produces blocks; the blocks produce
buildings and the lots inside them; the buildings produce homes and workplaces; the homes and
workplaces produce a hundred thousand daily schedules; the schedules produce traffic — and the
renderer only ever sees the result as instance buffers.

## 2. Layering

```
        Program.cpp / CliOptions        argv, run mode
                 |
             CityGame                   the CNA Game subclass: the loop, input, camera
        /        |         \
  Simulation   Renderers    Hud         fixed 30 Hz tick | per-frame draw | overlay
      |            |
  City model   Mesh + instance builders
```

Nothing below `CityGame` knows about the window, and nothing in the simulation knows about the
graphics device. That split is what makes `--bench` able to run the simulation at a scale the
renderer could not draw.

## 3. Determinism

Everything generated — the network, the buildings, the population, every agent's home, workplace
and schedule — comes from one 64-bit seed through a PCG32 stream, and every subsystem draws from
its own sub-stream so that changing the number of agents does not change the city they live in.

## 4. Where the frame goes *(planned)*

Filled in from `--bench` once the renderer exists.
