# Framework fixes found by CNA City

CNA City exists to bend CNA until something gives and then say precisely what gave. Eleven such
things are recorded in [`../CNA-FINDINGS.md`](../CNA-FINDINGS.md) as capability gaps, A1 to A11;
this directory holds the ones that have a *fix* rather than only a diagnosis, as patches against
`../../cnanext` and `../../sharp-runtimenext`.

They are patches rather than commits in those repositories on purpose. Both are separate checkouts
shared with other work — at the time of writing `cnanext` had uncommitted changes in
`modules/platform/` from another session, modified minutes earlier — and editing a tree somebody
else is building in breaks their build and risks sweeping their work into a commit that is not
theirs. A patch says exactly what to change and can be applied when the tree is quiet.

Each patch carries, in its own header:

- which finding it closes,
- what the defect actually is, checked against the engine's source rather than inferred,
- how to verify the fix from CNA City, because the program that found it is the program best
  placed to say whether it is gone.

## What is here

| file | finding | state |
|---|---|---|
| `a8-volumetric-fog-light.patch` | A8 — the fog pass is never given the scene's light | applies, compiles, not run |
| `a3-texture-mip-generation.patch` | A3 — `Texture2D` allocates a mip chain and says "generate" | applies, compiles, not run |
| `a6-parallel-for-partitioning.patch` | A6 — `Parallel::For` creates a thread per iteration | applies, compiles, **changes behaviour** |
| `a7-component-closure.patch.md` | A7 — widening the component closure is undiscoverable | a note, not a patch |

`a6` is against `../../sharp-runtimenext`; the rest are against `../../cnanext`. It is the one to
be careful with: the other two fill in a call that was missing, and that one changes how work is
partitioned across threads. Its header says what to run before landing it.

The seven with no patch here — A1 (lights or a texture set, never both), A2 (no attribute slot for
a per-instance colour), A4 (`AtmosphericSky` below the horizon), A5 (only transparency reports a
fallback), A9 (the engine layer writes GLSL, the Vulkan backend takes SPIR-V), A10 (only EasyGL has
a GPU timer query), A11 (Vulkan has one vertex binding, so no instancing) — are engine design
questions or whole unwritten backend features rather than defects with an obvious fix, and saying
so is more useful than guessing at one. A9 is somebody's decision about where a shader-language
boundary belongs; A11 already carries its own ticket in CNA's source. Three patches, one note,
seven left open: that is all eleven.

## Status against the current `next`

Re-checked on 2026-09-04 against `cnanext` `c9d8bfd` and `sharp-runtimenext` `c3fbb95`, which is
where both checkouts stood after the CNA work this project was waiting on landed:

| patch | target | applies |
|---|---|---|
| `a3-texture-mip-generation.patch` | cnanext `c9d8bfd` | cleanly |
| `a8-volumetric-fog-light.patch` | cnanext `c9d8bfd` | cleanly |
| `a6-parallel-for-partitioning.patch` | sharp-runtimenext `c3fbb95` | cleanly |

Checked with `git apply --check`, which modifies nothing; both trees were verified untouched
afterwards. **That is a weaker claim than it looks and is deliberately all that was checked.** It
says the surrounding code has not moved under these diffs. It does not say they still compile
against this revision, that their behaviour is still right, or that A6's threading semantics are
what that project wants today -- and finding out would mean applying them to a checkout other work
shares, which is the thing this directory exists to avoid.

So they remain candidates, not proposals. Whoever picks them up should apply each one to a quiet
tree, build it, run that project's own suite, and judge A6 hardest: it changes how work is
partitioned across threads and leaves the other `Parallel` overloads -- `ParallelLoopState`,
`ForEach` -- on the old behaviour, so it is a semantic decision for sharp-runtime rather than a
fix to accept on this project's say-so.

## Applying one

```sh
cd ../../cnanext
git apply --check ../cna-city/framework-fixes/<name>.patch   # refuses cleanly if it will not fit
git apply           ../cna-city/framework-fixes/<name>.patch
```

Then rebuild CNA City, which links CNA from source, and run the verification recipe in the patch
header.
