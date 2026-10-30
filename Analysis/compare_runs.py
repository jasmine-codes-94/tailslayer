#!/usr/bin/env python3
"""
compare_runs.py — Sanity test for our all-call code: run Laurie's benchmark and
analyze_all_call_runs on the same machine, cores and offset, and check the numbers agree.

Run both back to back, under the same scheduling (both with `sudo chrt -f 99`, or both
without), with the same cores, channel bit and sample count:

    cd discovery/benchmark && make
    sudo chrt -f 99 ./hedged_read_cpp --arm dual_quiet --core-a 11 --core-b 12 \
        --channel-bit 8 --samples 5000000 > laurie.csv
    cd ../../Analysis
    sudo chrt -f 99 ./analyze_all_call_runs 11 12 256 8 5000000 > ours.txt
    python3 compare_runs.py ../discovery/benchmark/laurie.csv ours.txt

Exits 0 if every check passes, 1 otherwise.
"""

import argparse
import csv
import re
import sys

PERCENTILES = ["p50", "p99", "p99.9", "p99.99", "max"]

# Laurie's CSV column for each percentile (her values are in TSC ticks)
LAURIE_COLUMNS = {"p50": "p50_cyc", "p99": "p99_cyc", "p99.9": "p999_cyc", "p99.99": "p9999_cyc", "max": "max_cyc"}

# Our table row and the matching row in Laurie's CSV
ROWS = [("replica A", "hedged_quiet_ch0"), ("replica B", "hedged_quiet_ch1"), ("hedged (min)", "hedged_quiet")]

# How far our percentiles may be from Laurie's. Two runs never match exactly, and the further
# into the tail, the fewer reads each percentile rests on (p99.99 of 5M reads is ~500 reads),
# so the tail gets more room. Max is a single read, so it's shown but not checked.
# These are starting points: tighten or loosen them once you've seen a few runs.
TOLERANCES = {"p50": 0.05, "p99": 0.10, "p99.9": 0.25, "p99.99": 0.25}

# How much lower our pairing rate may be than Laurie's. Ours starts the threads together,
# so if anything it should pair more
PAIRING_SLACK = 0.05


def verdict(passed):
    return "PASS" if passed else "FAIL"


def read_laurie(path):
    """Laurie's hedged_quiet rows converted to ns, plus her pairing rate, sample count and TSC GHz."""
    with open(path, newline="") as f:
        rows = {row["arm"]: row for row in csv.DictReader(f)}
    missing = [arm for _, arm in ROWS if arm not in rows]
    if missing:
        sys.exit(f"ERROR: {path} has no rows for {', '.join(missing)} (run her benchmark with --arm dual_quiet)")

    results = {}
    for label, arm in ROWS:
        ticks_per_ns = float(rows[arm]["tsc_ghz"])
        results[label] = {pct: int(rows[arm][column]) / ticks_per_ns for pct, column in LAURIE_COLUMNS.items()}

    hedged = rows["hedged_quiet"]
    n_samples = int(hedged["n_samples"])
    return results, int(hedged["n_paired"]) / n_samples, n_samples, float(hedged["tsc_ghz"])


def read_ours(path):
    """Our percentiles (already in ns), pairing rate, sample count and final-work call counts."""
    with open(path) as f:
        text = f.read()

    results = {}
    for line in text.splitlines():
        for label, _ in ROWS:
            if line.startswith(label):
                fields = line[len(label):].split()
                if fields[:2] == ["no", "samples"]:
                    sys.exit(f"ERROR: {path}: '{label}' has no samples")
                results[label] = dict(zip(PERCENTILES, map(float, fields[:5])))

    paired = re.search(r"Paired (\d+) of (\d+) reads", text)
    calls = re.search(r"Final work calls: replica A (\d+), replica B (\d+) \(each should be (\d+)\)", text)
    if len(results) != len(ROWS) or not paired or not calls:
        sys.exit(f"ERROR: {path} doesn't look like analyze_all_call_runs output")

    n_paired, n_samples = int(paired[1]), int(paired[2])
    pairing = n_paired / n_samples if n_samples else 0.0
    return results, pairing, n_samples, tuple(int(count) for count in calls.groups())


def main():
    parser = argparse.ArgumentParser(description="Sanity test: compare analyze_all_call_runs against Laurie's benchmark.")
    parser.add_argument("laurie_csv", help="stdout of: hedged_read_cpp --arm dual_quiet ...")
    parser.add_argument("ours_txt", help="stdout of: analyze_all_call_runs ...")
    args = parser.parse_args()

    laurie, laurie_pairing, laurie_n, tsc_ghz = read_laurie(args.laurie_csv)
    ours, ours_pairing, ours_n, (calls_a, calls_b, calls_expected) = read_ours(args.ours_txt)
    results = []  # One True/False per check

    if laurie_n != ours_n:
        print(f"WARNING: different sample counts (Laurie {laurie_n}, ours {ours_n}), so the tails won't line up as well\n")

    # Percentiles, side by side
    print(f"Percentiles in ns (Laurie's TSC ticks converted at her {tsc_ghz:.3f} GHz)\n")
    print(f"{'':14} {'':7} {'Laurie':>9} {'ours':>9} {'diff':>8} {'allowed':>8}")
    for label, _ in ROWS:
        for pct in PERCENTILES:
            theirs, mine = laurie[label][pct], ours[label][pct]
            diff = (mine - theirs) / theirs if theirs else float("inf")
            if pct in TOLERANCES:
                passed = abs(diff) <= TOLERANCES[pct]
                results.append(passed)
                allowed, result = f"±{TOLERANCES[pct]:.0%}", verdict(passed)
            else:
                allowed, result = "", "(not checked)"
            name = label if pct == PERCENTILES[0] else ""
            print(f"{name:14} {pct:7} {theirs:9.1f} {mine:9.1f} {diff:+8.1%} {allowed:>8}  {result}")
        print()

    # Our run did the all-call behaviour: no claim, so every thread called the final work every time
    passed = calls_a == calls_expected and calls_b == calls_expected
    results.append(passed)
    print(f"{verdict(passed)}  Every thread called the final work on every read: "
          f"A={calls_a} B={calls_b} (expected {calls_expected} each)")

    passed = ours_pairing >= laurie_pairing - PAIRING_SLACK
    results.append(passed)
    print(f"{verdict(passed)}  Pairing rate: Laurie {laurie_pairing:.1%}, ours {ours_pairing:.1%}")

    # In each run on its own, hedging should cut the tail: the min of two channels beats either one
    hedging_passed = []
    for name, run in (("Laurie", laurie), ("ours", ours)):
        tails = {label: run[label]["p99.9"] for label, _ in ROWS}
        passed = tails["hedged (min)"] < min(tails["replica A"], tails["replica B"])
        results.append(passed)
        hedging_passed.append(passed)
        print(f"{verdict(passed)}  Hedging cuts the tail ({name}): hedged p99.9 {tails['hedged (min)']:.1f} ns, "
              f"replicas {tails['replica A']:.1f} / {tails['replica B']:.1f} ns")

    failures = results.count(False)
    print(f"\n{len(results) - failures} of {len(results)} checks passed")
    if not any(hedging_passed):
        print("Hedging didn't help in either run: the replicas probably aren't on different channels on this "
              "machine (check channel_offset / channel_bit), rather than a bug in our code")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
