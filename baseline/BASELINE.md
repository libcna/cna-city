# The frozen baseline

What this build of CNA does with this city, recorded so that a later CNA can be measured against
it rather than described next to it.

```sh
scripts/baseline.sh verify quick     # a couple of minutes; belongs in CI on every commit
scripts/baseline.sh verify full      # adds the 100 000-citizen day and the quarter-million sweep
scripts/baseline.sh capture full     # re-freeze, after a change that is *meant* to move it
```

## Why digests and not milliseconds

A benchmark that freezes wall-clock numbers freezes the machine it ran on. Ten minutes later the
same build measures 0.43 ms and 1.25 ms for the same work because something else woke up, and the
baseline becomes a thing people learn to ignore.

So the frozen part is the **world digest**, which has no timing in it at all. It changes when the
city changes and at no other time, which makes `verify` a regression test with nothing to argue
about: either the same seed still produces the same city, or somebody changed the simulation. The
timings live in the benchmark reports (`--report`), where a spread is printed beside every figure
and a difference smaller than the spread is not called a change.

Five digests per scenario rather than one, because a mismatch should be a lead. A **city** that
differs means the generator moved; **agents** alone means the schedule or the steering did;
**traffic** alone means the road model; **transit** alone means the metro or the buses; **world**
alone means the clock or the weather.

## What is recorded

- `environment.txt` — the commits of cna-city, cnanext and sharp-runtimenext, the compiler, the
  build type, the renderer, and when it was taken. Every line in it is a reason a digest is allowed
  to differ, and the file exists so that "it changed" can be answered with "because of what".
- `checksums.txt` — one line per scenario, six digests each.
- `scenarios.txt` — the scenarios as command-line fields, so any one of them can be reproduced by
  hand without reading the script.

Each scenario is also checked *against itself* before it is frozen: `--checksum` re-runs it at half
the step size and on a different number of worker threads, and `capture` refuses to record a
scenario whose own re-runs disagree. Freezing a number this build cannot reproduce twice would be
worse than freezing nothing.

## The scenarios, and why each one is there

| scenario | seed | citizens | city | simulated | what it pins |
|---|---|---|---|---|---|
| `generator` | 20260902 | 2 000 | 3.3 km | 1 h | the city generator, which is a pure function of the seed |
| `oversubscribed` | 4242 | 8 000 | 1.24 km | 24 h | 93 buses over 20 stops: the configuration that found the bus defects in P21 |
| `morning-peak` | 20260902 | 100 000 | 3.3 km | 6 h | the peak, where the planner and the route pool are under most pressure |
| `whole-day` | 20260902 | 100 000 | 3.3 km | 24 h | the headline: a full day at the population the project is named for |
| `quarter-million` | 7 | 250 000 | 3.3 km | 2 h | past the point where the tick stops being about any one subsystem |

`full` is the better part of an hour, most of it `whole-day`: `--checksum` runs every scenario
three times, and the half-step re-run is twice the ticks on its own. That is the price of a
scenario that checks itself, and it is why the tiers exist.

The first two are the `quick` tier. They are the ones fast enough to run on every commit, and
between them they cover the generator and the transit model — which is where every defect found so
far has actually been.

## When the baseline moves

A deliberate change to the simulation moves these digests and is supposed to. The rule is not that
they never change; it is that a change is never a surprise. Moving one means:

1. a line in the log below saying which change moved it, which parts moved, and why that is right;
2. `scripts/baseline.sh capture full`;
3. both in the same commit as the change that caused it.

## Log

### 2026-09-04 — first freeze

Frozen after P21, at cna-city `60ab47b`, cnanext `c9d8bfd`, sharp-runtimenext `c3fbb95`. The state
being frozen is: 130 tests passing; `--checksum` reproducing at half the step size and on one
worker thread; a seven-day soak with 168 checkpoints, zero invariant violations, both frame models
agreeing on every digest, and no measurable accumulation in memory, route slots or queues.
