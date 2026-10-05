#!/usr/bin/env bash
# Times this simulator against libCacheSim on the same trace file, one
# (policy, cache size) configuration per process, so the number reported is
# single-configuration end-to-end throughput including trace decode.
#
# Usage:
#   benchmark_vs_libcachesim.sh TRACE REQUESTS CACHE_SIZE [POLICY...]
#
# Both tools read the same file and (for the nine policies that agree) produce
# identical miss counts, so the only thing differing is time.
set -uo pipefail

TRACE="${1:?usage: $0 TRACE REQUESTS CACHE_SIZE [POLICY...]}"
REQUESTS="${2:?}"
CACHE_SIZE="${3:?}"
shift 3
POLICIES=("$@")
[[ ${#POLICIES[@]} -eq 0 ]] && POLICIES=(LRU LFU Belady)

LCS_BIN="${LCS_BIN:-/home/user/libCacheSim/_build/bin/cachesim}"
BENCH_BIN="${BENCH_BIN:-/home/user/cache-simulator/build/tools/bench}"
WORK="${WORK:-/tmp/cachesim-bench}"
mkdir -p "$WORK"

# Give both tools the same page-cache starting conditions by reading the
# portion of the trace they will touch. Without this, whichever runs second
# gets a warm cache and looks faster.
BYTES=$(( REQUESTS * 24 ))
printf 'warming page cache over %s bytes of %s ...\n' "$BYTES" "$TRACE"
head -c "$BYTES" "$TRACE" > /dev/null 2>&1 || true

printf '\n%-10s %12s %14s %12s %12s %10s\n' policy requests "cache_size" "libCacheSim" "cachesim" "speedup"
printf '%s\n' "--------------------------------------------------------------------------------"

for policy in "${POLICIES[@]}"; do
  cat > "$WORK/one.yaml" <<YAML
trace:
  path: $TRACE
  type: oracleGeneral
  params: ""
  num_req: $REQUESTS
  sample_ratio: 1.0
global:
  ignore_obj_size: true
  consider_obj_metadata: false
  verbose: false
  print_head_req: false
output:
  path: $WORK/lcs_one.txt
configurations:
  - policy: $policy
    cache_size: $CACHE_SIZE
YAML
  rm -f "$WORK/lcs_one.txt"

  lcs_start=$(date +%s.%N)
  "$LCS_BIN" "$WORK/one.yaml" > "$WORK/lcs_stdout.txt" 2>&1
  lcs_rc=$?
  lcs_end=$(date +%s.%N)
  lcs_time=$(echo "$lcs_end - $lcs_start" | bc)
  lcs_miss=$(grep -oP 'n_miss \K[0-9]+' "$WORK/lcs_one.txt" 2>/dev/null | head -1)

  mine_start=$(date +%s.%N)
  "$BENCH_BIN" "$TRACE" --type oracleGeneral --policy "$policy" --size "$CACHE_SIZE" \
      --requests "$REQUESTS" --stream --csv > "$WORK/mine.csv" 2>"$WORK/mine_err.txt"
  mine_rc=$?
  mine_end=$(date +%s.%N)
  mine_time=$(echo "$mine_end - $mine_start" | bc)
  mine_miss=$(awk -F, 'NR==2{print $4}' "$WORK/mine.csv" 2>/dev/null)

  if [[ $lcs_rc -ne 0 || $mine_rc -ne 0 ]]; then
    printf '%-10s %12s %14s %12s %12s %10s  (libCacheSim rc=%s, cachesim rc=%s)\n' \
      "$policy" "$REQUESTS" "$CACHE_SIZE" "-" "-" "-" "$lcs_rc" "$mine_rc"
    continue
  fi

  speedup=$(echo "scale=2; $lcs_time / $mine_time" | bc)
  agree="="
  [[ "$lcs_miss" != "$mine_miss" ]] && agree="DIFFER($lcs_miss vs $mine_miss)"
  printf '%-10s %12s %14s %11.2fs %11.2fs %9sx  %s\n' \
    "$policy" "$REQUESTS" "$CACHE_SIZE" "$lcs_time" "$mine_time" "$speedup" "$agree"
done
