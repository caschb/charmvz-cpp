#!/bin/sh
# Runs one charmvz conversion under an enforced cgroup memory limit and
# reports what it cost.
#
#   tools/constrained_run.sh LIMIT BUDGET TRACE_DIR OUT_DIR SPILL_DIR [REFERENCE_OUT]
#
# LIMIT    cgroup v2 memory.max for the run (e.g. 1G); swap is disabled for the
#          scope, so exceeding it is an OOM kill rather than paging.
# BUDGET   charmvz --memory-budget (0 = unlimited).
# SPILL_DIR must be on disk. A tmpfs (such as /tmp on many desktops) is
#          memory: spilled runs there are charged to RAM, defeating the budget.
# REFERENCE_OUT, if given, is an unlimited run's output directory; the new
#          output must match it exactly (tools/compare_tables.py --exact).
#
# A cgroup and not `ulimit -v`: an address-space limit breaks Arrow's and
# zlib's mappings long before real memory use reaches it.
#
# Uses the binary in $CHARMVZ_BUILD (default builddir-rel) and prints one
# summary line of key=value pairs; the conversion's log is OUT_DIR.log.

set -eu

if [ $# -lt 5 ] || [ $# -gt 6 ]; then
  sed -n '2,20p' "$0" >&2
  exit 2
fi
limit=$1 budget=$2 trace=$3 out=$4 spill=$5 reference=${6:-}
here=$(cd "$(dirname "$0")" && pwd)
build=${CHARMVZ_BUILD:-$here/../builddir-rel}

if [ "$(stat -f -c %T "$spill")" = tmpfs ]; then
  echo "warning: $spill is on tmpfs; spilled runs will be charged to memory" >&2
fi

rm -rf "$out"
mkdir -p "$(dirname "$out")"
stats=$(mktemp)
trap 'rm -f "$stats"' EXIT

# Inside the scope: run charmvz under a getrusage wrapper, then read the
# scope's own cgroup counters before it is torn down. memory.peak includes
# page cache charged to the cgroup (output and spill writes), so it can sit
# at the limit while the process RSS stays well below it.
status=0
systemd-run --user --scope --quiet -p MemoryMax="$limit" -p MemorySwapMax=0 \
  -p OOMPolicy=continue \
  -- sh -c '
    stats=$1 out=$2; shift 2
    cg=/sys/fs/cgroup$(sed -n "s/^0:://p" /proc/self/cgroup)
    LD_LIBRARY_PATH=$(dirname "$1") python3 -I -c "
import resource, subprocess, sys, time
log = open(sys.argv[1] + \".log\", \"w\")
t0 = time.monotonic()
rc = subprocess.call(sys.argv[2:], stdout=log, stderr=subprocess.STDOUT)
wall = time.monotonic() - t0
rss = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss
print(f\"rc={rc} wall_s={wall:.2f} peak_rss_mib={rss / 1024:.1f}\")
sys.exit(rc)
" "$out" "$@" > "$stats"
    rc=$?
    echo "cgroup_peak_mib=$(( $(cat "$cg/memory.peak") / 1048576 ))" >> "$stats"
    echo "oom_kills=$(sed -n "s/^oom_kill //p" "$cg/memory.events")" >> "$stats"
    exit $rc
  ' sh "$stats" "$out" "$build/charmvz" -l "$trace" -o "$out" \
     --memory-budget "$budget" --spill-dir "$spill" || status=$?

result=$(tr '\n' ' ' < "$stats")
spilled=$(sed -n 's/.* \([0-9.]* [KMGT]*i*B\) written in total.*/\1/p' \
  "$out.log" | tr ' ' '_')
leftover=$(find "$spill" -maxdepth 1 -name 'charmvz-spill-*' | wc -l)

identical=n/a
if [ -n "$reference" ] && [ "$status" -eq 0 ]; then
  if uv run -q --with pyarrow python -I "$here/compare_tables.py" --exact \
    "$reference" "$out" > "$out.compare" 2>&1; then
    identical=yes
  else
    identical=no
  fi
fi

echo "trace=$(basename "$trace") limit=$limit budget=$budget ${result}spilled=${spilled:-0} spill_dirs_left=$leftover identical=$identical"
exit "$status"
