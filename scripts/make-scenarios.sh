#!/bin/sh
# Regenerates the benchmark scenarios.
#
# Measuring the morning peak means simulating up to the morning peak first, and at a hundred
# thousand citizens that is twenty-five seconds of warm-up before the first number -- paid again
# for every scale, every renderer and every run. Loading the same moment from a snapshot takes a
# quarter of a second.
#
# The files are not in git: they are twenty megabytes each and they are a pure function of the seed
# plus the warm-up, which is what this script is.
set -e
cd "$(dirname "$0")/.."
CITY=${CITY:-./build/cna-city}
OUT=${OUT:-bench-results/scenarios}
AGENTS=${AGENTS:-100000}
SEED=${SEED:-42}
mkdir -p "$OUT"

make_one() {
    name=$1; start=$2; warmup=$3; note=$4; shift 4
    printf '  %-16s ' "$name"
    "$CITY" --headless --agents "$AGENTS" --seed "$SEED" --time "$start" --simulate "$warmup" \
            --checksum --save "$OUT/$name.snapshot" --note "$note" "$@" >/dev/null 2>&1
    printf 'ok\n'
}

echo "cna-city scenarios -- $AGENTS citizens, seed $SEED, into $OUT"
make_one empty-night   1.0  1h "02:00, an empty city"
make_one morning-rush  5.0  2h "07:00, the morning peak"
make_one evening-rush 15.5  2h "17:30, the evening peak"
make_one rain-gridlock 7.0  2h "09:00 in the rain" --weather rain --fixed-weather
make_one metro-peak    6.0  2h "08:00, the underground at its busiest" --cars 0.15
echo "done"
