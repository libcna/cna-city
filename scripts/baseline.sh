#!/bin/sh
# Freezes, and re-checks, what this build of CNA does with this city.
#
# A benchmark number is only worth keeping if somebody can later tell whether a difference came
# from the thing being measured or from everything around it. So this records both halves: the
# exact commits of cna-city, CNA and sharp-runtime, the compiler and the build flags -- and a set
# of world digests that are a pure function of the simulation, with no timing in them at all.
#
# The digests are the point. A wall-clock number changes when the machine is busy; a digest changes
# only when the city does. That makes `verify` a regression test with no tolerance to argue about,
# and it makes the provenance beside it the thing that says *why* a digest was allowed to move.
#
#   scripts/baseline.sh capture [quick|full]   write baseline/ from this tree
#   scripts/baseline.sh verify  [quick|full]   re-run and diff against what is recorded
#
# `quick` is the two scenarios that finish in a couple of minutes and is what belongs in CI on
# every commit. `full` adds the hundred-thousand-citizen day and the quarter-million sweep, which
# are minutes each because --checksum deliberately runs every scenario three times: once as asked,
# once at half the step size, and once on a different number of worker threads.
set -e
cd "$(dirname "$0")/.."

BIN=${BIN:-./build/cna-city}
OUT=${OUT:-baseline}
CNA=${CNA:-../cnanext}
SHARP=${SHARP:-../sharp-runtimenext}

# name|seed|agents|half-size|metro|simulate  -- the fields cna-city takes on the command line, so
# a scenario can be reproduced by hand from this table alone.
QUICK_SCENARIOS='
generator|20260902|2000|1650|5|1h
oversubscribed|4242|8000|620|3|24h
'
FULL_SCENARIOS="$QUICK_SCENARIOS"'
morning-peak|20260902|100000|1650|5|6h
whole-day|20260902|100000|1650|5|24h
quarter-million|7|250000|3300|7|6h
'

action=${1:-verify}
tier=${2:-quick}
case "$tier" in
    quick) scenarios=$QUICK_SCENARIOS ;;
    full)  scenarios=$FULL_SCENARIOS ;;
    *)     echo "usage: $0 [capture|verify] [quick|full]" >&2; exit 2 ;;
esac

[ -x "$BIN" ] || { echo "$0: $BIN is not built" >&2; exit 2; }

sha() { git -C "$1" rev-parse HEAD 2>/dev/null || echo "unknown"; }
dirty() {
    n=$(git -C "$1" status --porcelain 2>/dev/null | wc -l)
    [ "$n" = "0" ] && echo clean || echo "$n uncommitted files"
}

write_environment() {
    cat <<EOF
# What these digests were produced by. Every line here is a reason a digest is allowed to differ.
cna-city         $(sha .) ($(dirty .))
cnanext          $(sha "$CNA") ($(dirty "$CNA"))
sharp-runtimenext $(sha "$SHARP") ($(dirty "$SHARP"))
compiler         $(${CXX:-c++} --version | head -1)
cmake            $(cmake --version | head -1)
build-type       $(grep -m1 '^CMAKE_BUILD_TYPE' build/CMakeCache.txt 2>/dev/null | cut -d= -f2)
renderer         $(grep -m1 '^CNA_GRAPHICS_RENDERER:' build/CMakeCache.txt 2>/dev/null | cut -d= -f2)
cnaext           $(grep -m1 '^CNA_CNAEXT:' build/CMakeCache.txt 2>/dev/null | cut -d= -f2)
captured         $(date -u '+%Y-%m-%dT%H:%M:%SZ')
EOF
}

# Runs one scenario and prints "name city agents traffic transit world final" on one line.
# Nothing here is timed: the whole point is a figure that does not move when the machine is busy.
run_scenario() {
    IFS='|' read -r name seed agents half metro simulate <<EOF
$1
EOF
    out=$("$BIN" --checksum --seed "$seed" --agents "$agents" --size "$half" \
                 --metro "$metro" --simulate "$simulate" 2>&1)
    # A scenario whose two re-runs disagree has no business being a baseline: it would freeze a
    # number that this build cannot reproduce even against itself.
    if echo "$out" | grep -q ' NO$'; then
        echo "$0: $name does not reproduce against itself:" >&2
        echo "$out" | sed -n '/reproduced/,$p' >&2
        exit 1
    fi
    digest() { echo "$out" | awk -v k="$1" '$1==k {print $2}'; }
    echo "$name $(digest CITY) $(digest AGENTS) $(digest TRAFFIC) $(digest TRANSIT) $(digest WORLD) $(digest FINAL)"
}

case "$action" in
capture)
    mkdir -p "$OUT"
    write_environment > "$OUT/environment.txt"
    {
        echo "# scenario city agents traffic transit world final"
        echo "# Reproduce one by hand:"
        echo "#   ./build/cna-city --checksum --seed S --agents N --size H --metro M --simulate D"
        echo "$scenarios" | while read -r line; do
            [ -z "$line" ] && continue
            echo "$line" | cut -d'|' -f1 | tr -d '\n' >&2; echo " ..." >&2
            run_scenario "$line"
        done
    } > "$OUT/checksums.txt"
    echo "$scenarios" | grep -v '^$' > "$OUT/scenarios.txt"
    echo "wrote $OUT/checksums.txt, $OUT/environment.txt and $OUT/scenarios.txt"
    ;;
verify)
    [ -f "$OUT/checksums.txt" ] || { echo "$0: no baseline in $OUT; run capture first" >&2; exit 2; }
    rm -f "$OUT/.verify-failed"
    # The loop runs in a subshell because of the pipe, so a shell variable set inside it does not
    # survive to the exit status below. The marker file is the state that does.
    echo "$scenarios" | while read -r line; do
        [ -z "$line" ] && continue
        name=$(echo "$line" | cut -d'|' -f1)
        want=$(grep "^$name " "$OUT/checksums.txt" || true)
        if [ -z "$want" ]; then
            echo "  $name: not in the baseline, skipped"
            continue
        fi
        got=$(run_scenario "$line")
        if [ "$got" = "$want" ]; then
            echo "  $name: unchanged"
        else
            # Named parts rather than one verdict: a city that differs means the generator moved,
            # agents alone means the schedule or the steering did, traffic alone means the road
            # model did. That is the difference between a failing check and a lead.
            echo "  $name: CHANGED"
            i=2
            for part in city agents traffic transit world final; do
                w=$(echo "$want" | cut -d' ' -f$i); g=$(echo "$got" | cut -d' ' -f$i)
                if [ "$w" != "$g" ]; then echo "      $part  was $w  now $g"; fi
                i=$((i + 1))
            done
            echo "$name" >> "$OUT/.verify-failed"
        fi
    done
    if [ -f "$OUT/.verify-failed" ]; then
        moved=$(tr '\n' ' ' < "$OUT/.verify-failed")
        rm -f "$OUT/.verify-failed"
        echo ""
        echo "The baseline moved: $moved"
        echo "That is not automatically wrong -- a deliberate change to the simulation moves these"
        echo "digests and is supposed to. It needs a line in baseline/BASELINE.md saying which"
        echo "change moved it and why, and a re-capture. What it must never be is a surprise."
        exit 1
    fi
    echo "baseline holds"
    ;;
*)
    echo "usage: $0 [capture|verify] [quick|full]" >&2; exit 2 ;;
esac
