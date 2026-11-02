#include "single_call.hpp"
#include "../discovery/benchmark/app_config.hpp"
#include "../discovery/benchmark/hw_utils.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>

static constexpr uint64_t STARTUP_MARGIN_CYCLES = 100000000; // Time for the threads to start and pin before round 0

// Startup only, never touched once the rounds begin
static std::atomic<int> m_threads_ready{0}; // Threads that are pinned

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

    m_threads_ready.fetch_add(1, std::memory_order_release);

    volatile char *addr = context->addr;

    // Warmup: the same rounds on the same schedule, CAS included, but not recorded, so cold caches,
    // TLB, branch predictor, clock ramp-up and a cold claim line don't end up in the samples
    const uint64_t *warmup_deadlines = context->warmup_deadlines.data();
    for (int i = 0; i < AppConfig::WARMUP_ITERS; i++) {
        HardwareUtils::clflush_addr(addr);
        HardwareUtils::mfence_inst();

        while (HardwareUtils::rdtsc_lfence() < warmup_deadlines[i]) {}

        (void)HardwareUtils::rdtsc_lfence();
        uint8_t val = *(volatile uint8_t *)addr;
        asm volatile("" :: "r"(val));

        uint64_t round = i;
        uint64_t expected = round;
        if (g_claim.round.compare_exchange_strong(expected, round + 1)) {
            (void)HardwareUtils::rdtscp_lfence();
        }
    }

    const uint64_t *deadlines = context->deadlines.data();
    round_sample *out = context->samples.data();
    int n_rounds = context->samples.size();
    uint64_t wins = 0; // Local, so counting causes no cache traffic

    for (int k = 0; k < n_rounds; k++) {
        // Evict our replica so the read has to go to DRAM
        HardwareUtils::clflush_addr(addr);
        HardwareUtils::mfence_inst();

        while (HardwareUtils::rdtsc_lfence() < deadlines[k]) {}

        uint64_t t0 = HardwareUtils::rdtsc_lfence();
        uint8_t val = *(volatile uint8_t *)addr; // The actual read of the data
        asm volatile("" :: "r"(val));

        // Warmup claimed rounds 0 to WARMUP_ITERS - 1, so timed round k is claim round WARMUP_ITERS + k
        // seq_cst, never relaxed: the CAS must not complete before the read returns (matters on ARM)
        uint64_t round = AppConfig::WARMUP_ITERS + k;
        uint64_t expected = round;
        bool won = g_claim.round.compare_exchange_strong(expected, round + 1);

        uint64_t t_call = 0;
        if (won) {
            t_call = HardwareUtils::rdtscp_lfence();
            wins++; // Stand-in for final_work(val)
        }

        out[k] = { t0, t_call, won };
    }

    context->wins = wins;
}

std::vector<race_context> race_reads(const std::vector<volatile char *>& addrs,
                                     const std::vector<int>& cores, int n_rounds, uint64_t period) {
    if (addrs.size() != cores.size()) {
        fprintf(stderr, "ERROR: race_reads needs one core per address (got %zu addresses, %zu cores)\n",
                addrs.size(), cores.size());
        return {};
    }

    std::vector<race_context> contexts(addrs.size());
    for (size_t i = 0; i < addrs.size(); i++) {
        contexts[i].addr = addrs[i];
        contexts[i].core_id = cores[i];
        // Allocated (and touched) up front, so no page faults mid-race
        contexts[i].warmup_deadlines.resize(AppConfig::WARMUP_ITERS);
        contexts[i].deadlines.resize(n_rounds);
        contexts[i].samples.resize(n_rounds);
    }

    // Every thread gets the same schedule, so they start each round together without communicating.
    // The warmup rounds come first, and the timed rounds carry on at the same spacing
    uint64_t t_start = HardwareUtils::rdtsc_lfence() + STARTUP_MARGIN_CYCLES;
    for (race_context& context : contexts) {
        for (int i = 0; i < AppConfig::WARMUP_ITERS; i++) {
            context.warmup_deadlines[i] = t_start + i * period;
        }
        for (int k = 0; k < n_rounds; k++) {
            context.deadlines[k] = t_start + (AppConfig::WARMUP_ITERS + k) * period;
        }
    }

    g_claim.round.store(0);
    m_threads_ready.store(0);

    std::vector<std::thread> threads;
    for (race_context& context : contexts) {
        threads.emplace_back(race_thread, &context);
    }

    // Wait until every thread is pinned, as in all_call (the deadlines, not this, line the threads up).
    // Yield on each check: the new threads start on main's core, and under chrt -f 99 they
    // can't run (to pin themselves elsewhere) unless main gives up the core
    while (m_threads_ready.load(std::memory_order_acquire) < static_cast<int>(threads.size())) {
        std::this_thread::yield();
    }

    for (std::thread& thread : threads) {
        thread.join();
    }

    return contexts;
}
