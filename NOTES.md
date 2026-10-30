# Notes

Work on adding an exactly-once claim to Tailslayer, so only one of the two racing threads runs the final work, and measuring what that costs.

## Known issues w / Claim

- **The previous round's winner gets a head start.** After winning, a thread holds the claim line in its own cache. If it wins again, its CAS is local and cheap; if the other thread wins, it pays a cross-core transfer. Expect `claim` latency to have two humps, and "Thread A won %" may lean away from 50/50 because of this, not because of DRAM. That's real behaviour, not a bug, but it matters when reading the results.
- **The channel printout ignores `--channel-offset`.** It always checks bit 8, so a different offset could misreport the channel. It's informational only.
- **`CORE_MAIN = 14` is still hardcoded** in `discovery/benchmark/app_config.hpp`, so on a machine with fewer than 15 logical CPUs the program exits at startup ("pin main to coordinator core"). Change it to a core that isn't one of the two workers.
- **`pair` and `claim` aren't quite the same harness.** `pair` reads back to back, while `claim` spaces reads out by the deadline period, so the DRAM row may have been closed in between. That could push `claim`'s baseline up slightly for reasons unrelated to the CAS.


## How to run the Analysis code

`Analysis/` holds the refactored version. Only the all-call analysis (no claim: both threads call the final work) is runnable so far. It needs the same one-time setup as above, then:

```bash
cd Analysis
g++ -O2 -std=c++17 -pthread -o analyze_all_call_runs analyze_all_call_runs.cpp all_call.cpp setup_environment.cpp
sudo chrt -f 99 ./analyze_all_call_runs <core_a> <core_b> <channel_offset> <channel_bit> <n_samples>
```

- All five arguments are required, in decimal, e.g. `11 12 256 8 5000000`. `channel_offset` and `channel_bit` are machine-specific and aren't checked.
- `-O2` is required: the timing helpers rely on the optimizer inlining them.
- It refuses to run unless both worker cores and main's core (`CORE_MAIN`, 14) exist, the two workers are separate physical cores that share an L3, and neither is on main's physical core.
- Output: how many times each thread called the final work (each should equal `n_samples`), how many reads were paired, and p50 / p99 / p99.9 / p99.99 / max in ns for replica A, replica B and the hedged min.
- Known issue: under `chrt -f 99` it currently hangs in `record_reads`. The workers start out on main's core and can't run there while main spins waiting for them. Until that's fixed (a `std::this_thread::yield()` in main's wait loop), run it without `chrt`.

## Sanity check: compare our code against Laurie's benchmark

`all_call` copies the measuring loop from Laurie's benchmark (her quiet arms), so on the same machine with the same settings, the two should give roughly the same numbers. `Analysis/compare_runs.py` checks that.

One-time setup on the Ubuntu box:

```bash
sudo apt install build-essential
echo 2 | sudo tee /sys/kernel/mm/hugepages/hugepages-1048576kB/nr_hugepages
grep Huge /proc/meminfo   # HugePages_Total should be 2
```

If the hugepage reservation fails because memory is too fragmented, add `default_hugepagesz=1G hugepagesz=1G hugepages=2` to the kernel boot options and reboot.

Build both, from the repo root:

```bash
(cd discovery/benchmark && make)
(cd Analysis && g++ -O2 -std=c++17 -pthread -o analyze_all_call_runs analyze_all_call_runs.cpp all_call.cpp setup_environment.cpp)
```

Pick two worker cores from `lscpu -e`: different `CORE` numbers, the same L3, and not on main's physical core (14). Ours refuses anything else; Laurie's doesn't check, so use cores ours accepts. Then run both back to back, with the same cores, channel bit and sample count:

```bash
sudo ./discovery/benchmark/hedged_read_cpp --arm dual_quiet --core-a 11 --core-b 12 --channel-bit 8 --samples 5000000 > laurie.csv
sudo ./Analysis/analyze_all_call_runs 11 12 256 8 5000000 > ours.txt
python3 Analysis/compare_runs.py laurie.csv ours.txt
```

- Laurie's `--channel-bit 8` sets her offset to 256 (2⁸), so ours gets offset `256` and bit `8`.
- Run both with `sudo`. Laurie's exits without root (it needs physical addresses), and ours prints a bogus channel for replica B without it.
- Both runs need the same scheduling. Until the `chrt` hang is fixed, run both without `chrt` (as above). Once it's fixed, put `chrt -f 99` in front of both.
- Each command's stdout holds only results (setup messages go to stderr), so `>` captures exactly what `compare_runs.py` parses.

What it checks (exit code 0 if everything passes, 1 if not):

- Laurie's `hedged_quiet_ch0`, `ch1` and `hedged_quiet` rows against our replica A, replica B and hedged min: p50 within 5%, p99 within 10%, p99.9 and p99.99 within 25%. Max is shown but not checked. The tolerances are guesses, set as constants at the top of the script.
- Each of our threads called the final work exactly `n_samples` times.
- Our pairing rate is no more than 5 points below hers.
- In each run, hedging cuts the tail: hedged p99.9 is below both replicas' p99.9.

Reading a failure:

- **p50 fails:** the runs didn't measure the same thing. Check that the cores and arguments matched, and that the CPU clock didn't change between runs (the `performance` governor helps).
- **Only p99.9 / p99.99 fail:** usually tail noise or a scheduling mismatch. Re-run both before concluding anything.
- **"Hedging cuts the tail" fails in both runs:** the replicas aren't on different channels on this machine (check the offset and bit). That's the setup, not our code.
- To see how much Laurie's numbers move on their own, run her benchmark twice and compare the two CSVs. That tells you whether the tolerances are about right for this machine.



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
