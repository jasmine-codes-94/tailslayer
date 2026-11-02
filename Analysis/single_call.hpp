#ifndef SINGLE_CALL_HPP
#define SINGLE_CALL_HPP

/*
 * single_call — Two threads race to read the same value from two replicas, and a CAS on a
 * shared claim makes sure only the winner calls the final work.
 * Rounds run on a fixed schedule of TSC deadlines, so both threads start each round together
 * without communicating. Recording only, like all_call.
 * Based on discovery/claim_race/enforced_single_call.cpp.
 */

#include <cstdint>
#include <vector>

struct round_sample {
    uint64_t t0;     // TSC when the read started
    uint64_t t_call; // Winner only: TSC just before calling the final work (0 for the loser)
    bool won;        // Whether this thread's CAS claimed the round
};

struct race_context {
    volatile char *addr;
    int core_id;
    std::vector<uint64_t> warmup_deadlines; // Start time (TSC) of each warmup round
    std::vector<uint64_t> deadlines;        // Start time (TSC) of each timed round: samples[k] belongs to deadlines[k]
    std::vector<round_sample> samples;      // This thread's result for each timed round
    uint64_t wins = 0;                      // How many timed rounds this thread won (and called the final work)
};

/*
Races one thread per address (addrs[i] read on cores[i]) for n_rounds timed rounds, period TSC ticks apart.
First runs AppConfig::WARMUP_ITERS (5000) warmup rounds on the same schedule, CAS included, that aren't recorded.
Returns an empty vector (after printing why) if addrs and cores aren't the same length.
*/
std::vector<race_context> race_reads(const std::vector<volatile char *>& addrs,
                                     const std::vector<int>& cores, int n_rounds, uint64_t period);

#endif // SINGLE_CALL_HPP
