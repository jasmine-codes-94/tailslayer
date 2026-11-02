/*
 * analyze_single_call_runs.cpp — Hedged reads with a claim: two threads each read their own
 * replica of the data, and a CAS on a shared claim lets only the winner call the final work.
 * Records a run with single_call, checks the final work ran exactly once per round, then reports
 * the winner's latency and how closely each thread kept to the round schedule.
 *
 * Build: g++ -O2 -std=c++17 -pthread -o analyze_single_call_runs analyze_single_call_runs.cpp single_call.cpp single_call_analysis.cpp setup_environment.cpp
 *        (-O2 is required: the timing helpers in hw_utils.hpp rely on the optimizer inlining them)
 * Run:   sudo chrt -f 99 ./analyze_single_call_runs <core_a> <core_b> <channel_offset> <channel_bit> <n_rounds> [period_ns]
 */

#include "single_call.hpp"
#include "single_call_analysis.hpp"
#include "setup_environment.hpp"
#include "../discovery/benchmark/app_config.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

static constexpr int DEFAULT_PERIOD_NS = 10000; //  (Laurie's p99.99 was ~10K ns)

// One row of the results table: the tail-latency percentiles of these latencies, in nanoseconds
static void print_row(const char *label, std::vector<uint64_t> latencies, double tsc_ticks_per_ns) {
    printf("%-18s", label);
    if (latencies.empty()) {
        printf(" no samples\n");
        return;
    }

    std::sort(latencies.begin(), latencies.end());
    size_t n = latencies.size();
    for (double fraction : {0.50, 0.99, 0.999, 0.9999, 1.0}) {
        uint64_t ticks = latencies[std::min(n - 1, static_cast<size_t>(n * fraction))];
        printf(" %9.1f", ticks / tsc_ticks_per_ns);
    }
    printf("   (n=%zu)\n", n);
}

int main(int argc, char *argv[]) {
    if (argc != 6 && argc != 7) {
        fprintf(stderr, "Usage: %s <core_a> <core_b> <channel_offset> <channel_bit> <n_rounds> [period_ns]\n", argv[0]);
        return 1;
    }
    int core_a = atoi(argv[1]);
    int core_b = atoi(argv[2]);
    int channel_offset = atoi(argv[3]);
    int channel_bit = atoi(argv[4]);
    int n_rounds = atoi(argv[5]);
    int period_ns = argc == 7 ? atoi(argv[6]) : DEFAULT_PERIOD_NS;

    Environment env;
    if (!setup_environment(core_a, core_b, channel_offset, channel_bit, env)) return 1;

    uint64_t period = static_cast<uint64_t>(period_ns * env.tsc_ticks_per_ns);
    fprintf(stderr, "Round period: %d ns (%lu TSC ticks)\n", period_ns, period);

    std::vector<race_context> contexts = race_reads({env.replica_a, env.replica_b}, {core_a, core_b}, n_rounds, period);
    if (contexts.empty()) return 1;
    const race_context& a = contexts[0];
    const race_context& b = contexts[1];

    round_analysis result = analyze_rounds(a, b);

    // Exactly once: one final work call per round, from whichever thread won it
    printf("Final work calls: replica A %lu, replica B %lu, total %lu (should be %d)\n",
           a.wins, b.wins, a.wins + b.wins, n_rounds);
    printf("Rounds not won exactly once: %d (should be 0)\n", result.not_exactly_once);
    printf("Late rounds: %d of %d (a thread started %d+ ticks after the deadline, so only one was racing)\n\n",
           result.late_rounds, n_rounds, AppConfig::MAX_PAIR_GAP);

    printf("%-18s %9s %9s %9s %9s %9s   (ns)\n", "", "p50", "p99", "p99.9", "p99.99", "max");
    print_row("t_call - t0", result.call_since_t0, env.tsc_ticks_per_ns);
    print_row("t_call - deadline", result.call_since_deadline, env.tsc_ticks_per_ns);
    print_row("A: t0 - deadline", result.start_a, env.tsc_ticks_per_ns);
    print_row("B: t0 - deadline", result.start_b, env.tsc_ticks_per_ns);

    teardown_environment(env);
    return 0;
}
