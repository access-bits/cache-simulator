#!/usr/bin/env python3
"""Runs this simulator and libCacheSim on the same trace and diffs them exactly.

Both tools compute the same deterministic function of a trace: given the trace
file, the policy and the cache size, the number of misses is determined. So a
difference is a bug in one of them, not a modelling choice -- which is what
makes this an effective test and not just a comparison.

It compares raw miss *counts*, not miss ratios. libCacheSim's own output prints
four decimal places, which at a billion requests leaves 50,000 misses of slack;
counts leave none. (The libCacheSim binary must be built from a tree whose
bin/cachesim/main.c prints n_req/n_miss -- see --patch-note.)

Usage:
  compare_libcachesim.py TRACE --type oracleGeneral \
      --policy LRU --policy LFU --policy Belady \
      --size 1000 --size 10000 [--requests N] [--ignore-obj-size]
"""

import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

PATCH_NOTE = """
libCacheSim prints miss ratios to 4 decimal places, which is not enough to
confirm an exact match. To get raw counts, in the libCacheSim tree edit
libCacheSim/bin/cachesim/main.c and extend the result line's snprintf with:

    ", n_req %lld, n_miss %lld, n_req_byte %lld, n_miss_byte %lld",
    ..., (long long)result[i].n_req, (long long)result[i].n_miss,
    (long long)result[i].n_req_byte, (long long)result[i].n_miss_byte

then rebuild the cachesim target. Without it, this script falls back to
comparing the 4-decimal ratios and says so.
"""


def run_libcachesim(binary, trace, trace_type, jobs, requests, ignore_obj_size, out_dir):
    """Returns {(policy, size): (n_req, n_miss) or ('ratio', value)}."""
    config = out_dir / "compare.yaml"
    results_path = out_dir / "libcachesim_results.txt"
    lines = [
        "trace:",
        f"  path: {trace}",
        f"  type: {trace_type}",
        '  params: ""',
        f"  num_req: {requests if requests else -1}",
        "  sample_ratio: 1.0",
        "",
        "global:",
        f"  ignore_obj_size: {'true' if ignore_obj_size else 'false'}",
        "  consider_obj_metadata: false",
        "  verbose: false",
        "  print_head_req: false",
        "",
        "output:",
        f"  path: {results_path}",
        "",
        "configurations:",
    ]
    for policy, size in jobs:
        lines.append(f"  - policy: {policy}")
        lines.append(f"    cache_size: {size}")
    config.write_text("\n".join(lines) + "\n")

    if results_path.exists():
        results_path.unlink()
    completed = subprocess.run(
        [str(binary), str(config)], capture_output=True, text=True, check=False
    )
    if completed.returncode != 0:
        print(completed.stdout[-4000:], file=sys.stderr)
        print(completed.stderr[-4000:], file=sys.stderr)
        raise SystemExit(f"libCacheSim exited {completed.returncode}")

    text = results_path.read_text() if results_path.exists() else completed.stdout
    exact = re.compile(
        r"^\S+\s+(\S+)\s+cache size\s+(\d+)(\S*),\s+\d+ req, miss ratio ([\d.]+)"
        r"(?:.*?n_req (\d+), n_miss (\d+))?",
        re.MULTILINE,
    )
    out = {}
    order = []
    for match in exact.finditer(text):
        name, size, unit, ratio, n_req, n_miss = match.groups()
        size = int(size)
        # The result line scales the size by a display unit; undo it.
        scale = {"B": 1, "KiB": 1024, "MiB": 1024**2, "GiB": 1024**3}.get(unit, 1)
        key = (name, size * scale)
        value = (int(n_req), int(n_miss)) if n_miss is not None else ("ratio", float(ratio))
        order.append((key, value))
    # libCacheSim emits results in config order, so pair them up positionally:
    # that survives a policy appearing at several sizes and a name it spells
    # differently from the config.
    for (policy, size), (_, value) in zip(jobs, order):
        out[(policy, size)] = value
    return out


def run_cachesim(binary, trace, trace_type, jobs, requests, ignore_obj_size):
    policies, sizes = [], []
    for policy, size in jobs:
        if policy not in policies:
            policies.append(policy)
        if size not in sizes:
            sizes.append(size)
    command = [str(binary), str(trace), "--type", trace_type, "--csv"]
    for policy in policies:
        command += ["--policy", policy]
    for size in sizes:
        command += ["--size", str(size)]
    if requests:
        command += ["--requests", str(requests)]
    if ignore_obj_size:
        command.append("--ignore-obj-size")
    completed = subprocess.run(command, capture_output=True, text=True, check=False)
    if completed.returncode != 0:
        print(completed.stderr[-4000:], file=sys.stderr)
        raise SystemExit(f"cachesim bench exited {completed.returncode}")
    out = {}
    for line in completed.stdout.splitlines():
        parts = line.split(",")
        if len(parts) < 5 or parts[0] == "policy":
            continue
        out[(parts[0], int(parts[1]))] = (int(parts[2]), int(parts[3]))
    return out, completed.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("trace")
    parser.add_argument("--type", default="oracleGeneral")
    parser.add_argument("--policy", action="append", default=[])
    parser.add_argument("--size", action="append", type=int, default=[])
    parser.add_argument("--requests", type=int, default=0)
    parser.add_argument("--ignore-obj-size", action="store_true")
    parser.add_argument("--libcachesim",
                        default="/home/user/libCacheSim/_build/bin/cachesim")
    parser.add_argument("--bench",
                        default="/home/user/cache-simulator/build/tools/bench")
    parser.add_argument("--patch-note", action="store_true",
                        help="print how to make libCacheSim report exact counts")
    args = parser.parse_args()

    if args.patch_note:
        print(PATCH_NOTE)
        return 0

    policies = args.policy or ["LRU", "LFU", "Belady"]
    sizes = args.size or [1000, 10000]
    jobs = [(policy, size) for policy in policies for size in sizes]

    with tempfile.TemporaryDirectory() as tmp:
        theirs = run_libcachesim(args.libcachesim, args.trace, args.type, jobs,
                                 args.requests, args.ignore_obj_size, Path(tmp))
    mine, _ = run_cachesim(args.bench, args.trace, args.type, jobs, args.requests,
                           args.ignore_obj_size)

    approximate = any(v[0] == "ratio" for v in theirs.values())
    if approximate:
        print("WARNING: libCacheSim reported only 4-decimal ratios; comparison is "
              "approximate. Run with --patch-note for how to fix this.\n")

    print(f"{'policy':<12}{'cache_size':>12}{'n_req':>14}"
          f"{'libCacheSim':>14}{'cachesim':>14}{'delta':>10}  verdict")
    print("-" * 92)
    mismatches = 0
    missing = 0
    for key in jobs:
        policy, size = key
        theirs_value = theirs.get(key)
        mine_value = mine.get(key)
        if theirs_value is None or mine_value is None:
            print(f"{policy:<12}{size:>12}{'?':>14}{'missing':>14}{'missing':>14}"
                  f"{'-':>10}  NO DATA")
            missing += 1
            continue
        n_req, mine_miss = mine_value
        if theirs_value[0] == "ratio":
            theirs_miss = round(theirs_value[1] * n_req)
            tolerance = max(1, int(5e-5 * n_req))
        else:
            _, theirs_miss = theirs_value
            tolerance = 0
        delta = mine_miss - theirs_miss
        ok = abs(delta) <= tolerance
        if not ok:
            mismatches += 1
        print(f"{policy:<12}{size:>12}{n_req:>14}{theirs_miss:>14}{mine_miss:>14}"
              f"{delta:>10}  {'MATCH' if ok else 'MISMATCH'}")

    print()
    if mismatches == 0 and missing == 0:
        kind = "within rounding" if approximate else "exactly"
        print(f"All {len(jobs)} configurations agree {kind}.")
        return 0
    print(f"{mismatches} mismatch(es), {missing} missing, out of {len(jobs)}.")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
