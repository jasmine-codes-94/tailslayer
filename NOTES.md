# Notes

Work on adding an exactly-once claim to Tailslayer, so only one of the two racing threads runs the final work, and measuring what that costs.

## Known issues

- **The previous round's winner gets a head start.** After winning, a thread holds the claim line in its own cache. If it wins again, its CAS is local and cheap; if the other thread wins, it pays a cross-core transfer. Expect `claim` latency to have two humps, and "Thread A won %" may lean away from 50/50 because of this, not because of DRAM. That's real behaviour, not a bug, but it matters when reading the results.
- **The channel printout ignores `--channel-offset`.** It always checks bit 8, so a different offset could misreport the channel. It's informational only.
- **`CORE_MAIN = 14` is still hardcoded** in `discovery/benchmark/app_config.hpp`, so on a machine with fewer than 15 logical CPUs the program exits at startup ("pin main to coordinator core"). Change it to a core that isn't one of the two workers.
- **`pair` and `claim` aren't quite the same harness.** `pair` reads back to back, while `claim` spaces reads out by the deadline period, so the DRAM row may have been closed in between. That could push `claim`'s baseline up slightly for reasons unrelated to the CAS.

## How to run

One-time setup on the Ubuntu box:

```bash
sudo apt install build-essential
echo 2 | sudo tee /sys/kernel/mm/hugepages/hugepages-1048576kB/nr_hugepages
grep Huge /proc/meminfo   # HugePages_Total should be 2
```

If the hugepage reservation fails at runtime because memory is too fragmented, add `default_hugepagesz=1G hugepagesz=1G hugepages=2` to the kernel boot options and reboot.

Pick two cores from `lscpu -e`: different `CORE` numbers, same L3. Then:

```bash
cd discovery/claim_race
make
sudo chrt -f 99 ./claim_race --all --core-a N --core-b M > results.csv
```

- `--arm pair` or `--arm claim` runs just one arm.
- The CSV goes to stdout and the human-readable report to stderr.
- `--raw-prefix run1` also dumps every latency to `run1_<label>.csv`.
- Other options: `--rounds` (default 1,000,000) and `--period` (default 10,000 cycles).

What to look at:

1. The correctness block should say `OK: exactly one thread ran the final work every round`.
2. Compare the `claim` row with the `pair` row, especially p50 against p99.9 and up. If the claim adds a fixed cost at the median while the tail stays well below a single-channel read, the idea works.



## Background notes

- Pin the worker threads to cores that share an L3 (on AMD, the same CCX/CCD). The atomic claim's cost is a core-to-core cache-line transfer: ~20–50 ns within a shared L3 vs ~80–130 ns across CCDs via the IO die, which is about as much as a normal DRAM read.
- Split lock: an atomic that straddles two cache lines falls back to locking the whole bus, which is very slow. Fix: alignas(64).
- False sharing: don't put the flag on the same line as the replica data or any other hot variable. Give it a line to itself.
- Eviction: the line could fall out of cache if it sits unused for a long time while you stream lots of data. Touching it during the wait_work spin keeps it warm. If both threads only read it, both hold it in shared state, and the winner's RMW just has to invalidate the other copy, which is still a cache-only operation.
- clflush: the benchmark flushes the cache line before every read. Don't do that to the flag.
- Reset traffic: clearing the flag after each round is another write that moves the line. You can avoid it with a round counter: each round, CAS(flag, round-1, round). Whoever succeeds wins, and no reset is needed.


## Summary of changes

Everything is new code in `discovery/claim_race/` (`claim_race.cpp` + `Makefile`). None of the existing library or benchmark files were modified. It builds against the benchmark's `benchmark.cpp`, `stats.cpp` and `hw_utils.hpp`. It has not been compiled or run yet.

**Two arms:**

- `pair`: the original Tailslayer method, calling `Benchmark::run_arm` unchanged. The threads loop freely, reads that started within 400 cycles of each other are paired afterwards, and the min of each pair is taken.
- `claim`: a real race. Both threads start each round together, and only the winner runs the final work.

**The claim:** a single `std::atomic<uint64_t>` holding the last round claimed, `alignas(128)` so it has a cache line to itself (with no neighbour for Intel's adjacent-line prefetcher to pull in). Round `k` is won by whoever moves it from `k` to `k+1` with a `seq_cst` CAS, so it never needs a reset. It must stay `seq_cst`, not relaxed: on ARM a relaxed CAS could complete before the read returns.

**The round schedule:** `deadlines[k] = t_start + k * period` is computed once, before the threads start. Each thread spins on `rdtsc` until its deadline, so the two start together without communicating. A shared "go" flag would itself be a cache-line transfer. No jitter: refresh isn't regular enough for a fixed period to keep hitting the same point in the refresh cycle.

**Each round, per thread:**

```
flush replica; mfence
spin until deadlines[k]
t0 → read → CAS → if (won) { t_call; wins++ }
```

`wins` is a local variable in each thread, written out once after the loop, and stands in for `final_work(val)`.

**Core checks (all hard errors):** the program refuses to run if `--core-a` and `--core-b` are the same core, are hyperthreads of the same physical core, don't share an L3, or if it can't read the layout from sysfs.
