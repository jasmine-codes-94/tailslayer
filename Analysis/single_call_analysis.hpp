#ifndef SINGLE_CALL_ANALYSIS_HPP
#define SINGLE_CALL_ANALYSIS_HPP

/*
 * single_call_analysis — Turns a single_call run (both threads' rounds) into the numbers
 * analyze_single_call_runs prints. Kept apart from the recording so it can be tested
 * without hugepages, pinning or root (see test_single_call_analysis.cpp).
 */

#include "single_call.hpp"

#include <cstdint>
#include <vector>

struct round_analysis {
    int late_rounds = 0;                       // Rounds where a thread started MAX_PAIR_GAP+ ticks after the deadline
    int not_exactly_once = 0;                  // Rounds with no winner, or two
    std::vector<uint64_t> call_since_t0;       // Winner's t_call - t0, on-time rounds
    std::vector<uint64_t> call_since_deadline; // Winner's t_call - deadline, on-time rounds
    std::vector<uint64_t> start_a, start_b;    // Each thread's t0 - deadline, every round
};

/*
Goes through the rounds of threads a and b, which ran on the same schedule.
A round is late when a thread wasn't there at the deadline: it started its read MAX_PAIR_GAP or
more ticks after it, almost always because it was still busy with the previous round. Then only
one thread was really racing, so late rounds are left out of the winner's latencies (as all_call
leaves out reads it couldn't pair), but kept in the t0 - deadline rows, which show the schedule.
Rounds not won exactly once are counted and left out of the winner's latencies too.
*/
round_analysis analyze_rounds(const race_context& a, const race_context& b);

#endif // SINGLE_CALL_ANALYSIS_HPP
