# Framework fixes found by CNA City

CNA City exists to bend CNA until something gives and then say precisely what gave. Nine such
things are recorded in [`../CNA-FINDINGS.md`](../CNA-FINDINGS.md); this directory holds the ones
that have a *fix* rather than only a diagnosis, as patches against `../../cnanext` and
`../../sharp-runtimenext`.

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

The five findings with no patch here — A1 (lights or a texture set, never both), A2 (no attribute
slot for a per-instance colour), A4 (`AtmosphericSky` below the horizon), A5 (only transparency
reports a fallback) — are engine design questions rather than defects with an obvious fix, and
saying so is more useful than guessing at one.

## Applying one

```sh
cd ../../cnanext
git apply --check ../cna-city/framework-fixes/<name>.patch   # refuses cleanly if it will not fit
git apply           ../cna-city/framework-fixes/<name>.patch
```

Then rebuild CNA City, which links CNA from source, and run the verification recipe in the patch
header.
