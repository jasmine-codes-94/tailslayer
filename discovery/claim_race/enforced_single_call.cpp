/*
 * enforced_single_call.cpp — Two threads race to read the same value from two replicas.
 * A CAS on a shared counter makes sure only the winner calls the final work.
 *
 * No main: call race_reads() from a file that does the setup and analysis.
 */

#include "../benchmark/hw_utils.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

static constexpr uint64_t STARTUP_MARGIN_CYCLES = 100000000; // Time for the threads to start and pin before round 0

struct round_sample {
    uint64_t t0;     // Read started
    uint64_t t_call; // Winner only: about to call the final work (0 for the loser)
    bool won;
};

struct race_context {
    volatile char *addr;
    int core_id;
    std::vector<uint64_t> deadlines;   // Start time (TSC) of each round
    std::vector<round_sample> samples; // This thread's result for each round
    uint64_t wins = 0;                 // How many times this thread called the final work
};

/*
Holds the last round claimed. Round k is won by whoever moves it from k to k+1,
so it never needs resetting between rounds.
alignas(128): its own cache line, with no neighbour for the adjacent-line prefetcher to pull in.
*/
struct alignas(128) Claim {
    std::atomic<uint64_t> round{0};
};
static Claim g_claim;

static void race_thread(race_context *context) {
    if (HardwareUtils::pin_to_core(context->core_id) != 0) {
        perror("race_thread: sched_setaffinity");
        std::exit(1);
    }

    volatile char *addr = context->addr;
    const uint64_t *deadlines = context->deadlines.data();
    round_sample *out = context->samples.data();
    uint64_t n_rounds = context->samples.size();
    uint64_t wins = 0; // Local, so counting causes no cache traffic

    for (uint64_t k = 0; k < n_rounds; k++) {
        // Evict our replica so the read has to go to DRAM
        HardwareUtils::clflush_addr(addr);
        HardwareUtils::mfence_inst();

        while (HardwareUtils::rdtsc_lfence() < deadlines[k]) {}

        uint64_t t0 = HardwareUtils::rdtsc_lfence();
        uint8_t val = *(volatile uint8_t *)addr; // The actual read of the data
        asm volatile("" :: "r"(val));

        // seq_cst, never relaxed: the CAS must not complete before the read returns (matters on ARM)
        uint64_t expected = k;
        bool won = g_claim.round.compare_exchange_strong(expected, k + 1);

        uint64_t t_call = 0;
        if (won) {
            t_call = HardwareUtils::rdtscp_lfence();
            wins++; // Stand-in for final_work(val)
        }

        out[k] = { t0, t_call, won };
    }

    context->wins = wins;
}

/*
Races two threads, pinned to core_a and core_b, reading addr_a and addr_b (two replicas
in a hugepage, on different DRAM channels) once per round, period cycles apart.
Returns each thread's context with its samples and win count.
*/
std::array<race_context, 2> race_reads(volatile char *addr_a, volatile char *addr_b,
                                       int core_a, int core_b, int n_rounds, uint64_t period) {
    std::array<race_context, 2> contexts;
    contexts[0].addr = addr_a;
    contexts[0].core_id = core_a;
    contexts[1].addr = addr_b;
    contexts[1].core_id = core_b;

    // Allocated (and touched) before the clock starts, so no page faults mid-race
    for (race_context& context : contexts) {
        context.deadlines.resize(n_rounds);
        context.samples.resize(n_rounds);
    }

    // Both threads get the same schedule, so they start each round together without communicating
    uint64_t t_start = HardwareUtils::rdtsc_lfence() + STARTUP_MARGIN_CYCLES;
    for (race_context& context : contexts) {
        for (int k = 0; k < n_rounds; k++) {
            context.deadlines[k] = t_start + k * period;
        }
    }

    g_claim.round.store(0);

    std::thread thread_a(race_thread, &contexts[0]);
    std::thread thread_b(race_thread, &contexts[1]);
    thread_a.join();
    thread_b.join();

    return contexts;
}
