/*
 * test_single_call_analysis.cpp — Hand-built rounds through analyze_rounds, checking the
 * latencies, late rounds and the exactly-once count. Needs no hugepages, pinning or root,
 * so it builds and runs on any machine.
 *
 * Build: g++ -O2 -std=c++17 -o test_single_call_analysis test_single_call_analysis.cpp single_call_analysis.cpp
 * Run:   ./test_single_call_analysis   (exit code 0 if every check passes)
 */

#include "single_call_analysis.hpp"
#include "../discovery/benchmark/app_config.hpp"

#include <cstdint>
#include <cstdio>
#include <vector>

static int g_failures = 0;

// Prints the failed check and its line, and carries on so one run shows every failure
#define CHECK(...)                                                                   \
    do {                                                                             \
        if (!(__VA_ARGS__)) {                                                        \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #__VA_ARGS__);   \
            g_failures++;                                                            \
        }                                                                            \
    } while (0)

using ticks = std::vector<uint64_t>;

// Adds one round to a thread's results, as race_thread would have recorded it
static void add_round(race_context& context, uint64_t deadline, uint64_t t0, uint64_t t_call, bool won) {
    context.deadlines.push_back(deadline);
    context.samples.push_back({ t0, t_call, won });
}

// Both threads on time, A wins: the round counts, with the winner's latencies
static void test_on_time_round() {
    race_context a{}, b{};
    add_round(a, 1000, 1010, 1310, true);
    add_round(b, 1000, 1020, 0, false);

    round_analysis result = analyze_rounds(a, b);
    CHECK(result.late_rounds == 0);
    CHECK(result.not_exactly_once == 0);
    CHECK(result.call_since_t0 == ticks{300});       // 1310 - 1010
    CHECK(result.call_since_deadline == ticks{310}); // 1310 - 1000
    CHECK(result.start_a == ticks{10});
    CHECK(result.start_b == ticks{20});
}

// B starts just under, then exactly at, MAX_PAIR_GAP ticks late: only the second round is late,
// so it's left out of the winner's latencies but still shows in t0 - deadline
static void test_late_round() {
    const uint64_t gap = AppConfig::MAX_PAIR_GAP;
    race_context a{}, b{};
    add_round(a, 1000, 1000, 1200, true);
    add_round(b, 1000, 1000 + gap - 1, 0, false);
    add_round(a, 50000, 50000, 50200, true);
    add_round(b, 50000, 50000 + gap, 0, false);

    round_analysis result = analyze_rounds(a, b);
    CHECK(result.late_rounds == 1);
    CHECK(result.not_exactly_once == 0);
    CHECK(result.call_since_t0 == ticks{200}); // Round 0 only
    CHECK(result.start_a == ticks{0, 0});
    CHECK(result.start_b == ticks{gap - 1, gap});
}

// Two winners, then none, then a normal round: the first two are counted and left out
static void test_not_exactly_once() {
    race_context a{}, b{};
    add_round(a, 1000, 1000, 1100, true);
    add_round(b, 1000, 1000, 1150, true);
    add_round(a, 2000, 2000, 0, false);
    add_round(b, 2000, 2000, 0, false);
    add_round(a, 3000, 3000, 0, false);
    add_round(b, 3000, 3005, 3405, true);

    round_analysis result = analyze_rounds(a, b);
    CHECK(result.not_exactly_once == 2);
    CHECK(result.late_rounds == 0);
    CHECK(result.call_since_t0 == ticks{400});       // Round 2 only: 3405 - 3005
    CHECK(result.call_since_deadline == ticks{405}); // 3405 - 3000
    CHECK(result.start_b.size() == 3);               // Every round is still in the schedule rows
}

int main() {
    test_on_time_round();
    test_late_round();
    test_not_exactly_once();

    if (g_failures == 0) {
        printf("All checks passed\n");
        return 0;
    }
    printf("%d check(s) failed\n", g_failures);
    return 1;
}
