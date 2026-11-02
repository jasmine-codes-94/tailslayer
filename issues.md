# Issues

- **`analyze_all_call_runs` should take only `channel_bit`.** It should derive `channel_offset = 1 << channel_bit` instead of taking both separately, and fail if the replicas land on the same channel, so a mismatched pair can't silently measure one channel twice.
- **Check that the claim's CAS isn't optimized away.** The C++ standard doesn't forbid optimizing atomics in general (only `volatile` accesses are guaranteed), so after building `analyze_single_call_runs`, confirm with `objdump -d analyze_single_call_runs | grep "lock cmpxchg"` that the warmup and timed loops both still contain the CAS.
