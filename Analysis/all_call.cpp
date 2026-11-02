#include "all_call.hpp"
#include "../discovery/benchmark/app_config.hpp"
#include "../discovery/benchmark/hw_utils.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>

// Startup only, never touched once the timed reads begin
static std::atomic<int> m_threads_ready{0};       // Threads that are pinned and waiting
static std::atomic<bool> m_measure_signal{false}; // Releases them all at once

static void record_thread(record_context *context) {
    if (HardwareUtils::pin_to_core(context->core_id) != 0) {
        perror("record_thread: sched_setaffinity");
        std::exit(1);
    }

    m_threads_ready.fetch_add(1, std::memory_order_release);
    while (!m_measure_signal.load(std::memory_order_acquire)) {}

    volatile char *addr = context->addr;

    // Warmup, as in the benchmark: the same reads, timed but not recorded, so cold caches,
    // TLB, branch predictor and clock ramp-up don't end up in the samples
    for (int i = 0; i < AppConfig::WARMUP_ITERS; i++) {
        HardwareUtils::clflush_addr(addr);
        HardwareUtils::mfence_inst();
        HardwareUtils::lfence_inst();
        (void)HardwareUtils::rdtsc_lfence();
        uint8_t val = *(volatile uint8_t *)addr;
        asm volatile("" :: "r"(val));
        (void)HardwareUtils::rdtscp_lfence();
    }

    read_sample *out = context->samples.data();
    int n_samples = context->samples.size();
    uint64_t final_iterations = 0; // Local, so counting causes no cache traffic

    for (int i = 0; i < n_samples; i++) {
        // Evict our replica so the read has to go to DRAM
        HardwareUtils::clflush_addr(addr);
        HardwareUtils::mfence_inst();
        HardwareUtils::lfence_inst();

        uint64_t t0 = HardwareUtils::rdtsc_lfence();
        uint8_t val = *(volatile uint8_t *)addr; // The actual read of the data
        asm volatile("" :: "r"(val));
        uint64_t t1 = HardwareUtils::rdtscp_lfence();

        // The compiler may collapse this into a single `final_iterations = n_samples` after the
        // loop, since nothing else depends on it. The count stays right, but it's no longer
        // per-read work. To force it on every read: asm volatile("" : "+r"(final_iterations));
        final_iterations++; // Stand-in for final_work(val): every thread calls it on every read

        out[i] = { t0, t1 - t0 };
    }

    context->final_iterations = final_iterations;
}

std::vector<record_context> record_reads(const std::vector<volatile char *>& addrs,
                                         const std::vector<int>& cores, int n_samples) {
    if (addrs.size() != cores.size()) {
        fprintf(stderr, "ERROR: record_reads needs one core per address (got %zu addresses, %zu cores)\n",
                addrs.size(), cores.size());
        return {};
    }

    std::vector<record_context> contexts(addrs.size());
    for (size_t i = 0; i < addrs.size(); i++) {
        contexts[i].addr = addrs[i];
        contexts[i].core_id = cores[i];
        contexts[i].samples.resize(n_samples); // Allocated (and touched) up front, so no page faults mid-run
    }

    m_threads_ready.store(0);
    m_measure_signal.store(false);

    std::vector<std::thread> threads;
    for (record_context& context : contexts) {
        threads.emplace_back(record_thread, &context);
    }

    // Wait until every thread is pinned and spinning, so they really do start together.
    // Yield on each check: the new threads start on main's core, and under chrt -f 99 they
    // can't run (to pin themselves elsewhere) unless main gives up the core
    while (m_threads_ready.load(std::memory_order_acquire) < static_cast<int>(threads.size())) {
        std::this_thread::yield();
    }
    m_measure_signal.store(true, std::memory_order_release);

    for (std::thread& thread : threads) {
        thread.join();
    }

    return contexts;
}
