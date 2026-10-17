#!/bin/sh
# Runs charmvz_vis entry points one at a time, each under an enforced cgroup
# memory limit, and prints one line per entry point.
#
#   tools/constrained_viz.sh [--bins N] LIMIT TRACE_DIR [FUNCTION...]
#
# LIMIT     cgroup v2 memory.max per entry point (e.g. 2G); swap is disabled
#           for the scope, so exceeding it is an OOM kill rather than paging.
# FUNCTION  names from `tools/memprobe.py TRACE_DIR list`; all of them by
#           default.
# --bins N  passed to memprobe.py (time-bin width = run length / N).
#
# The entry points run strictly one after another: two large-trace queries in
# parallel would each see the other's page cache and CPU contention, and on a
# machine without much headroom over LIMIT could push each other out.
#
# POLARS_MAX_THREADS is honoured and reported on every line. Refuses to run
# when polars' temporary directory (POLARS_TEMP_DIR, else TMPDIR, else /tmp)
# is a tmpfs: anything spilled there is charged to memory.
#
# A line reads "NAME ok ... oom_kills=0" on success and
# "NAME FAILED rc=N oom_kills=N: <last stderr line>" otherwise. The exit
# status is 1 when any entry point failed.

set -eu

bins=
if [ "${1:-}" = --bins ]; then
  bins="--bins $2"
  shift 2
fi
if [ $# -lt 2 ]; then
  sed -n '2,23p' "$0" >&2
  exit 2
fi
limit=$1 trace=$2
shift 2
here=$(cd "$(dirname "$0")" && pwd)

tmp=${POLARS_TEMP_DIR:-${TMPDIR:-/tmp}}
if [ "$(stat -f -c %T "$tmp")" = tmpfs ]; then
  echo "error: polars temp dir $tmp is on tmpfs; set POLARS_TEMP_DIR to a directory on disk" >&2
  exit 2
fi

# Resolve the interpreter once, outside the limited scope, so uv's own
# resolution and sync are neither charged to the limit nor repeated.
py=$(cd "$here/.." && uv run --quiet python -c 'import sys; print(sys.executable)')
if [ $# -eq 0 ]; then
  # shellcheck disable=SC2046
  set -- $("$py" -I "$here/memprobe.py" "$trace" list)
fi

err=$(mktemp)
trap 'rm -f "$err"' EXIT
threads=${POLARS_MAX_THREADS:-default}
failed=0
for f in "$@"; do
  # The shell inside the scope survives an OOM kill of the probe (the kernel
  # picks the largest process), so it can still read the scope's counters.
  # shellcheck disable=SC2086
  out=$(systemd-run --user --scope --quiet -p MemoryMax="$limit" -p MemorySwapMax=0 \
    -p OOMPolicy=continue \
    -- sh -c '
      err=$1; shift
      cg=/sys/fs/cgroup$(sed -n "s/^0:://p" /proc/self/cgroup)
      rc=0
      "$@" 2>"$err" || rc=$?
      echo "rc=$rc oom_kills=$(sed -n "s/^oom_kill //p" "$cg/memory.events")"
    ' sh "$err" "$py" -I "$here/memprobe.py" "$trace" "$f" $bins) || true
  probe=$(printf '%s\n' "$out" | sed '$d' | tail -n 1)
  status=$(printf '%s\n' "$out" | tail -n 1)
  rc=$(printf '%s' "$status" | sed -n 's/^rc=\([0-9]*\).*/\1/p')
  ooms=$(printf '%s' "$status" | sed -n 's/.*oom_kills=\([0-9]*\).*/\1/p')
  if [ "$rc" = 0 ]; then
    echo "limit=$limit threads=$threads $probe oom_kills=$ooms"
  else
    failed=1
    echo "limit=$limit threads=$threads $f FAILED rc=$rc oom_kills=$ooms: $(tail -n 1 "$err")"
  fi
done
exit "$failed"
