#!/bin/sh
# Measures the same city through several of CNA's renderers and writes one comparison page.
#
# This is the test cna-city exists to make possible: identical seed, hour, weather, camera and
# population, put through OPENGLES3, OPENGL33, VULKAN and the rest, with the differences in one
# table. What it finds is not only "which is faster" -- it is which of them draws the same picture,
# which of them silently falls back on a pass, and which of them has a pass that costs ten times
# what it costs elsewhere.
#
# It is not run by default and it is not cheap: each renderer is a separate configure and a full
# rebuild of CNA, which is minutes of compilation and a few hundred megabytes of object files.
# That is why it reuses ONE build directory -- build-probe, the workspace's shared scratch build --
# rather than leaving a tree per renderer behind. The consequence is that the renderers are
# measured one after another and the tree is rebuilt between them; the alternative was five trees
# nobody ever deletes.
set -e
cd "$(dirname "$0")/.."

RENDERERS=${RENDERERS:-"OPENGLES3 OPENGL33"}
OUT=${OUT:-bench-results/renderers}
SCALES=${SCALES:-1000,10000,100000}
REPEAT=${REPEAT:-3}
PROBE=build-probe

mkdir -p "$OUT"
for renderer in $RENDERERS; do
    echo "=== $renderer"
    cmake -S . -B "$PROBE" \
          -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
          -DCMAKE_C_COMPILER_LAUNCHER=ccache \
          -DCNA_GRAPHICS_RENDERER="$renderer" \
          -DCNA_CITY_BUILD_TESTS=OFF >/dev/null
    cmake --build "$PROBE" -j"$(nproc)" --target cna-city >/dev/null
    # A renderer that cannot open a device on this machine is a result, not a failure: it goes in
    # the summary as "no device" rather than stopping the sweep.
    if ! "./$PROBE/cna-city" --report "$OUT/$(echo "$renderer" | tr 'A-Z' 'a-z')" \
                             --scales "$SCALES" --repeat "$REPEAT"; then
        echo "  $renderer: no device on this machine"
    fi
done

DIRS=""
for renderer in $RENDERERS; do
    dir="$OUT/$(echo "$renderer" | tr 'A-Z' 'a-z')"
    [ -f "$dir/simulation.csv" ] && DIRS="$DIRS $dir"
done
# shellcheck disable=SC2086
./build/cna-city --compare $DIRS --comparison-out "$OUT/comparison.html"
echo "wrote $OUT/comparison.html"
