/*
 * analyze_all_call_runs.cpp — Hedged reads with no claim: two threads each read their own
 * replica of the data, and both call the final work every time. Records a run with all_call,
 * then compares each replica's read latency with the hedged latency (the faster of the two).
 *
 * Build: g++ -O2 -std=c++17 -pthread -o analyze_all_call_runs analyze_all_call_runs.cpp all_call.cpp setup_environment.cpp
 *        (-O2 is required: the timing helpers in hw_utils.hpp rely on the optimizer inlining them)
 * Run:   sudo chrt -f 99 ./analyze_all_call_runs <core_a> <core_b> <channel_offset> <channel_bit> <n_samples>
 */

#include "all_call.hpp"
#include "setup_environment.hpp"
#include "../discovery/benchmark/app_config.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

/*
Pairs reads from the two threads that started within AppConfig::MAX_PAIR_GAP ticks of each
other, and keeps the faster of each pair: the latency a hedged read would have had.
Walks both lists in time order; a read with no partner close enough is skipped.
Same method as Benchmark::pair_samples_n in the original benchmark.
*/
static std::vector<uint64_t> pair_and_take_min(const std::vector<read_sample>& a, const std::vector<read_sample>& b) {
    std::vector<uint64_t> hedged;
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        uint64_t time_a = a[i].timestamp;
        uint64_t time_b = b[j].timestamp;
        uint64_t gap = time_a > time_b ? time_a - time_b : time_b - time_a;

        if (gap < static_cast<uint64_t>(AppConfig::MAX_PAIR_GAP)) {
            hedged.push_back(std::min(a[i].latency, b[j].latency));
            i++;
            j++;
        } else if (time_a < time_b) {
            i++; // A is behind in time, so move it forward
        } else {
            j++;
        }
    }
    return hedged;
}

static std::vector<uint64_t> latencies_of(const std::vector<read_sample>& samples) {
    std::vector<uint64_t> latencies;
    latencies.reserve(samples.size());
    for (const read_sample& sample : samples) {
        latencies.push_back(sample.latency);
    }
    return latencies;
}

// One row of the results table: the tail-latency percentiles of these latencies, in nanoseconds
static void print_row(const char *label, std::vector<uint64_t> latencies, double tsc_ticks_per_ns) {
    printf("%-14s", label);
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
    if (argc != 6) {
        fprintf(stderr, "Usage: %s <core_a> <core_b> <channel_offset> <channel_bit> <n_samples>\n", argv[0]);
        return 1;
    }
    int core_a = atoi(argv[1]);
    int core_b = atoi(argv[2]);
    int channel_offset = atoi(argv[3]);
    int channel_bit = atoi(argv[4]);
    int n_samples = atoi(argv[5]);

    Environment env;
    if (!setup_environment(core_a, core_b, channel_offset, channel_bit, env)) return 1;

    std::vector<record_context> contexts = record_reads({env.replica_a, env.replica_b}, {core_a, core_b}, n_samples);
    const std::vector<read_sample>& samples_a = contexts[0].samples;
    const std::vector<read_sample>& samples_b = contexts[1].samples;

    // No claim, so every thread calls the final work on every read
    printf("Final work calls: replica A %lu, replica B %lu (each should be %d)\n",
           contexts[0].final_iterations, contexts[1].final_iterations, n_samples);

    std::vector<uint64_t> hedged = pair_and_take_min(samples_a, samples_b);
    printf("Paired %zu of %d reads (started within %d ticks of each other)\n\n",
           hedged.size(), n_samples, AppConfig::MAX_PAIR_GAP);

    printf("%-14s %9s %9s %9s %9s %9s   (ns)\n", "", "p50", "p99", "p99.9", "p99.99", "max");
    print_row("replica A", latencies_of(samples_a), env.tsc_ticks_per_ns);
    print_row("replica B", latencies_of(samples_b), env.tsc_ticks_per_ns);
    print_row("hedged (min)", hedged, env.tsc_ticks_per_ns);

    teardown_environment(env);
    return 0;
}
