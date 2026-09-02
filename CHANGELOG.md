# Changelog

All notable changes to CNA City are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); the project has not cut a release yet, so
everything lives under *Unreleased*.

## [Unreleased]

### Added
- Project scaffolding: `README.md`, `LICENSE` (MIT), `.gitignore`, `.gitattributes`, this
  changelog and the architecture note.
- **City generation.** A planar road graph built by cutting polylines against each other -- an
  arterial grid, a boulevard ring, diagonal avenues and a rotated street grid per district -- whose
  bounded faces are the city blocks. Zoning, perimeter-block building placement, towers with
  setbacks, suburban plots, industrial sheds, parkland, street furniture and planting. 24 ms for a
  3.3 km city with 204 km of road and 11 808 buildings.
- **Simulation.** A hundred thousand citizens in a struct-of-arrays store with homes, workplaces
  and 24-hour schedules; two-level route planning with a direct-mapped cache; IDM car-following
  with signalised junctions; hashed-grid crowd separation; an underground with timetabled trains;
  a solar-position clock and a weather model that drives sky, fog, wetness and mode choice.
- **Rendering.** Chunked static geometry for the city and `InstancedRendererEXT` for its crowds,
  with procedural PBR materials generated at start-up -- there is no asset file in the project.
  Cascaded shadow maps, the analytic atmospheric sky, HDR with ACES, bloom, FXAA, height and
  volumetric fog, light shafts, and wet-surface response.
- **Five camera modes**, including a chase camera that follows one named citizen through their
  entire day, and a debug overlay for the road network and live routes.
- **Screen-space ambient occlusion**, filled from a `DepthNormalPrepass` the game draws itself --
  it is a contract, not a switch.
- **`--bench`**, a scale sweep with no graphics device at all, and `--headless` for a full
  simulated day.

### Measured
On an AMD Radeon 780M with 16 threads, at a hundred thousand citizens: the city generates from one
seed in 24 ms, the simulation tick averages 2.29 ms (p99 3.70 ms) in 24.5 MB, and the frame runs at
75 fps from a city overview and 107-145 fps at street level, at 1600x900 with four shadow cascades,
HDR, bloom and FXAA. A hundred times the agents costs 8.1 times the tick, because the route cache's
hit rate rises with population from 8% to 38%. Full tables and the defects found along the way are
in [`ARCHITECTURE.md`](ARCHITECTURE.md).
