#ifndef ALL_CALL_HPP
#define ALL_CALL_HPP

/*
 * all_call — The quiet arms of the original benchmark, recording only.
 * One thread per address, each timing reads of its own replica in a tight loop.
 * There's no claim, so in a real hedged read every thread would call the final work.
 * Pass one address for single_quiet, or two for hedged_quiet.
 */

#include <cstdint>
#include <vector>

struct read_sample {
    uint64_t timestamp; // TSC when the read started
    uint64_t latency;   // TSC ticks the read took
};

struct record_context {
    volatile char *addr;
    int core_id;
    std::vector<read_sample> samples; // This thread's reads, in order
    uint64_t final_iterations = 0;    // How many times this thread called the final work (one per read)
};

/*
Records n_samples timed reads of each address (addrs[i] read on cores[i]), all threads starting together.
Each thread first does AppConfig::WARMUP_ITERS (5000) warmup reads that aren't recorded, as in the benchmark.
Returns an empty vector (after printing why) if addrs and cores aren't the same length.
*/
std::vector<record_context> record_reads(const std::vector<volatile char *>& addrs,
                                         const std::vector<int>& cores, int n_samples);

#endif // ALL_CALL_HPP
