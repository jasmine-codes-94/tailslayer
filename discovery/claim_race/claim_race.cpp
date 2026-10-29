/*
 * claim_race.cpp — Cost of an exactly-once claim on top of a hedged read
 *
 * Two threads, each pinned to its own core, read their own replica of the same byte
 * (on different DRAM channels). Two arms:
 *
 *   pair   The original Tailslayer method (Benchmark::run_arm): each thread loops
 *          freely, and reads that started within MAX_PAIR_GAP cycles are paired
 *          offline, taking the min. Both threads would run the final work.
 *   claim  The threads genuinely race: both start round k at the same TSC deadline,
 *          which each computes on its own, so the start signal causes no cache
 *          traffic between the cores. After its read, each thread tries to claim the
 *          round with a CAS on a shared, cache-resident counter. Only the winner
 *          runs the final work.
 *
 * The headline comparison is pair's hedged read vs claim's call latency (read start to
 * the winner calling the final work). Both windows start at t0 and end on rdtscp;lfence.
 *
 * Build: make
 * Run:   sudo chrt -f 99 ./claim_race --all
 */

#include "../benchmark/app_config.hpp"
#include "../benchmark/benchmark.hpp"
#include "../benchmark/hw_utils.hpp"
#include "../benchmark/stats.hpp"

#include <atomic>
#include <thread>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cinttypes>
#include <string>
#include <getopt.h>
#include <sys/mman.h>

static constexpr int DEFAULT_ROUNDS = 1000000;
static constexpr int WARMUP_ROUNDS = 5000;
static constexpr uint64_t DEFAULT_PERIOD = 10000;       // Cycles between round deadlines
static constexpr uint64_t MAX_START_LATENESS = 1000;    // Starting later than this (cycles) invalidates the round
static constexpr double STARTUP_MARGIN_NS = 50000000.0; // 50ms for the threads to spawn and pin before round 0

/*
The shared claim. Holds the number of the last round that was claimed.
Round k is won by whoever moves it from k to k+1, so it never needs resetting.
alignas(128) gives it a cache line to itself with no neighbour either
(Intel's adjacent-line prefetcher pulls lines in 128-byte pairs).
*/
struct alignas(128) Claim {
    std::atomic<uint64_t> round{0};
};
static Claim g_claim;

struct round_sample {
    uint64_t t0;     // Read started
    uint64_t t_call; // Winner only: about to call the final work (0 for the loser)
    bool won;
};

struct race_context {
    volatile char *addr;
    int core_id;
    int n_rounds;
    const uint64_t *deadlines; // Start time (TSC) of each round, read-only
    round_sample *samples;
    uint64_t wins; // Output: how many times this thread ran the final work
};

static void claim_thread(race_context *context) {
    if (HardwareUtils::pin_to_core(context->core_id) != 0) {
        perror("claim_thread: sched_setaffinity");
        std::exit(1); // The other thread would win every round, so the run is useless
    }

    volatile char *addr = context->addr;
    const uint64_t *deadlines = context->deadlines;
    round_sample *out = context->samples;
    uint64_t wins = 0; // Local to this thread, so counting causes no cache traffic

    for (int k = 0; k < context->n_rounds; k++) {
        uint64_t deadline = deadlines[k];

        // Evict our replica before the round starts so the read has to go to DRAM
        HardwareUtils::clflush_addr(addr);
        HardwareUtils::mfence_inst();

        while (HardwareUtils::rdtsc_lfence() < deadline) {}

        uint64_t t0 = HardwareUtils::rdtsc_lfence();
        uint8_t val = *(volatile uint8_t *)addr; // The actual read of the data
        asm volatile("" :: "r"(val));

        // Claim round k. Must stay seq_cst (never relaxed) so the CAS can't complete
        // before the read above returns. x86's lock prefix guarantees that anyway; ARM doesn't
        uint64_t expected = k;
        bool won = g_claim.round.compare_exchange_strong(expected, k + 1);

        uint64_t t_call = 0;
        if (won) {
            t_call = HardwareUtils::rdtscp_lfence();
            wins++; // Stand-in for final_work(val): cheap, but countable
        }

        out[k] = { t0, t_call, won };
    }

    context->wins = wins;
}

static void report(const Stats& stats, const char *label, int n_rounds, std::vector<uint64_t>& data) {
    percentiles p = stats.compute_percentiles(data);
    stats.print_percentiles(label, data.size(), p);
    stats.emit_csv_row(label, n_rounds, data.size(), p);
    stats.dump_raw_latencies(label, data);
}

static void analyze(const Stats& stats, const std::vector<round_sample>& a, const std::vector<round_sample>& b,
                    uint64_t wins_a, uint64_t wins_b, int n_rounds, const std::vector<uint64_t>& deadlines) {
    std::vector<uint64_t> call_latency, start_skew;
    int checked = 0, late = 0, both_won = 0, neither_won = 0, a_wins = 0;

    for (int k = WARMUP_ROUNDS; k < n_rounds; k++) {
        const round_sample& ra = a[k];
        const round_sample& rb = b[k];
        uint64_t deadline = deadlines[k];

        // Correctness is checked on every round, late or not
        checked++;
        if (ra.won && rb.won) { both_won++; continue; }
        if (!ra.won && !rb.won) { neither_won++; continue; }

        // Timing only counts if both threads actually started together
        if (ra.t0 - deadline > MAX_START_LATENESS || rb.t0 - deadline > MAX_START_LATENESS) {
            late++;
            continue;
        }

        const round_sample& winner = ra.won ? ra : rb;
        if (ra.won) a_wins++;

        call_latency.push_back(winner.t_call - winner.t0);
        start_skew.push_back(ra.t0 > rb.t0 ? ra.t0 - rb.t0 : rb.t0 - ra.t0);
    }

    // The win counters cover every round, warmup included
    uint64_t total_wins = wins_a + wins_b;
    uint64_t final_claim = g_claim.round.load();
    bool ok = total_wins == static_cast<uint64_t>(n_rounds) && both_won == 0 && neither_won == 0
              && final_claim == static_cast<uint64_t>(n_rounds);

    fprintf(stderr, "\n=== claim: CORRECTNESS ===\n");
    fprintf(stderr, "  Final work ran:      A=%" PRIu64 " B=%" PRIu64 " total=%" PRIu64 " (must be %d)\n",
            wins_a, wins_b, total_wins, n_rounds);
    fprintf(stderr, "  Both threads won:    %d (must be 0, after warmup)\n", both_won);
    fprintf(stderr, "  Neither thread won:  %d (must be 0, after warmup)\n", neither_won);
    fprintf(stderr, "  Final claim value:   %" PRIu64 " (must be %d)\n", final_claim, n_rounds);
    fprintf(stderr, "  %s\n", ok ? "OK: exactly one thread ran the final work every round" : "FAILED");

    int valid = call_latency.size();
    fprintf(stderr, "\n=== claim: RACE ===\n");
    fprintf(stderr, "  Valid rounds:   %d (%d dropped for a late start)\n", valid, late);
    if (valid == 0) return;
    fprintf(stderr, "  Thread A won:   %.1f%%\n", 100.0 * a_wins / valid);

    report(stats, "claim", checked, call_latency);          // Winner's t_call - t0: read start -> final work called
    report(stats, "claim_start_skew", checked, start_skew); // |t0_a - t0_b|: should be small
}

static void run_claim(const Stats& stats, volatile char *replica_a, volatile char *replica_b,
                      int core_a, int core_b, int n_rounds, uint64_t period, double tsc_ghz) {
    fprintf(stderr, "\n--- Starting arm: claim (%d rounds, %" PRIu64 " cycles apart, ~%.1f s) ---\n",
            n_rounds, period, n_rounds * period / (tsc_ghz * 1e9));

    g_claim.round.store(0);

    // Value-initialised, so every page is touched now rather than faulting mid-race
    std::vector<round_sample> samples_a(n_rounds), samples_b(n_rounds);

    /*
    The round schedule, computed once up front. Both threads get the same deadline for
    round k, so they start it together without communicating.
    No jitter needed: refresh isn't regular enough for a fixed period to keep landing
    reads at the same point in the refresh cycle.
    */
    uint64_t t_start = HardwareUtils::rdtsc_lfence() + static_cast<uint64_t>(STARTUP_MARGIN_NS * tsc_ghz);
    std::vector<uint64_t> deadlines(n_rounds);
    for (int k = 0; k < n_rounds; k++) {
        deadlines[k] = t_start + k * period;
    }

    race_context ctx_a = { replica_a, core_a, n_rounds, deadlines.data(), samples_a.data(), 0 };
    race_context ctx_b = { replica_b, core_b, n_rounds, deadlines.data(), samples_b.data(), 0 };

    std::thread thread_a(claim_thread, &ctx_a);
    std::thread thread_b(claim_thread, &ctx_b);
    thread_a.join();
    thread_b.join();

    analyze(stats, samples_a, samples_b, ctx_a.wins, ctx_b.wins, n_rounds, deadlines);
}

static bool setup_replicas(int channel_offset, void **page, volatile char **replica_a, volatile char **replica_b) {
    *page = mmap(nullptr, AppConfig::SUPERPAGE_SIZE, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | (30 << MAP_HUGE_SHIFT), -1, 0);
    if (*page == MAP_FAILED) {
        perror("mmap 1GB hugepage (replicas)");
        return false;
    }

    std::memset(*page, 0x42, AppConfig::SUPERPAGE_SIZE);
    mlock(*page, AppConfig::SUPERPAGE_SIZE);

    *replica_a = static_cast<volatile char *>(*page);
    *replica_b = *replica_a + channel_offset;

    // Informational only: needs root to read physical addresses
    volatile char *replicas[2] = { *replica_a, *replica_b };
    for (int i = 0; i < 2; ++i) {
        uint64_t phys = HardwareUtils::virt_to_phys(reinterpret_cast<uint64_t>(replicas[i]));
        if (phys == 0) {
            fprintf(stderr, "replica_%d: virt=%p (physical address unavailable, run as root)\n",
                    i, (void *)replicas[i]);
        } else {
            fprintf(stderr, "replica_%d: virt=%p phys=0x%" PRIx64 " channel=%d\n", i, (void *)replicas[i],
                    phys, HardwareUtils::compute_channel(phys, AppConfig::DEFAULT_CHANNEL_BIT));
        }
    }
    return true;
}

/*
The CPUs that share this core's L3, as the kernel lists them (e.g. "0-7,16-23"),
or "" if it can't be read. Two cores share an L3 when their lists match.
*/
static std::string l3_shared_cpus(int core) {
    char path[128];
    for (int index = 0; ; ++index) {
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cache/index%d/level", core, index);
        FILE *f = fopen(path, "r");
        if (!f) return ""; // Ran out of cache levels without finding an L3
        int level = 0;
        bool read_ok = fscanf(f, "%d", &level) == 1;
        fclose(f);
        if (!read_ok || level != 3) continue;

        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cache/index%d/shared_cpu_list", core, index);
        f = fopen(path, "r");
        if (!f) return "";
        char buf[256] = {};
        if (!fgets(buf, sizeof(buf), f)) buf[0] = '\0';
        fclose(f);
        buf[strcspn(buf, "\n")] = '\0';
        return buf;
    }
}

/*
The claim's cost is a cache-line transfer between the two cores, which is much cheaper
when they share an L3 (on AMD: same CCX). Warn rather than fail, since comparing
across L3s is a legitimate experiment too.
*/
static void check_shared_l3(int core_a, int core_b) {
    std::string l3_a = l3_shared_cpus(core_a);
    std::string l3_b = l3_shared_cpus(core_b);
    if (l3_a.empty() || l3_b.empty()) {
        fprintf(stderr, "WARNING: couldn't read the L3 topology for cores %d and %d\n", core_a, core_b);
    } else if (l3_a != l3_b) {
        fprintf(stderr, "WARNING: cores %d and %d do NOT share an L3 (%s vs %s), so the claim crosses CCXs/CCDs\n",
                core_a, core_b, l3_a.c_str(), l3_b.c_str());
    } else {
        fprintf(stderr, "Cores %d and %d share an L3 (CPUs %s)\n", core_a, core_b, l3_a.c_str());
    }
}

static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s [options]\n", prog);
    fprintf(stderr, "  --all                 Run both arms\n");
    fprintf(stderr, "  --arm ARM             Run a specific arm (repeatable):\n");
    fprintf(stderr, "                          pair  (original offline pairing)\n");
    fprintf(stderr, "                          claim (lockstep + CAS claim)\n");
    fprintf(stderr, "  --rounds N            Rounds / samples per arm (default: %d, first %d are warmup)\n",
            DEFAULT_ROUNDS, WARMUP_ROUNDS);
    fprintf(stderr, "  --period N            Cycles between rounds (default: %" PRIu64 ")\n", DEFAULT_PERIOD);
    fprintf(stderr, "  --core-a N            Core for thread A (default: %d)\n", AppConfig::CORE_MEAS_A);
    fprintf(stderr, "  --core-b N            Core for thread B (default: %d)\n", AppConfig::CORE_MEAS_B);
    fprintf(stderr, "  --channel-offset N    Bytes between replicas (default: %d)\n", AppConfig::DEFAULT_CHANNEL_OFFSET);
    fprintf(stderr, "  --raw-prefix P        Dump raw latencies to P_<label>.csv\n");
    fprintf(stderr, "  --help                Show this help\n");
}

int main(int argc, char *argv[]) {
    bool do_pair = false, do_claim = false;
    int n_rounds = DEFAULT_ROUNDS;
    uint64_t period = DEFAULT_PERIOD;
    int core_a = AppConfig::CORE_MEAS_A;
    int core_b = AppConfig::CORE_MEAS_B;
    int channel_offset = AppConfig::DEFAULT_CHANNEL_OFFSET;
    std::string_view raw_prefix = "";

    static struct option long_opts[] = {
        {"all",            no_argument,       nullptr, 'a'},
        {"arm",            required_argument, nullptr, 'A'},
        {"rounds",         required_argument, nullptr, 'n'},
        {"period",         required_argument, nullptr, 'p'},
        {"core-a",         required_argument, nullptr, '1'},
        {"core-b",         required_argument, nullptr, '2'},
        {"channel-offset", required_argument, nullptr, 'O'},
        {"raw-prefix",     required_argument, nullptr, 'R'},
        {"help",           no_argument,       nullptr, 'h'},
        {nullptr, 0, nullptr, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "aA:n:p:1:2:O:R:h", long_opts, nullptr)) != -1) {
        switch (opt) {
        case 'a': do_pair = do_claim = true; break;
        case 'A':
            if (strcmp(optarg, "pair") == 0) do_pair = true;
            else if (strcmp(optarg, "claim") == 0) do_claim = true;
            else { usage(argv[0]); return 1; }
            break;
        case 'n': n_rounds = atoi(optarg); break;
        case 'p': period = strtoull(optarg, nullptr, 0); break;
        case '1': core_a = atoi(optarg); break;
        case '2': core_b = atoi(optarg); break;
        case 'O': channel_offset = atoi(optarg); break;
        case 'R': raw_prefix = optarg; break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 1;
        }
    }

    if ((!do_pair && !do_claim) || n_rounds <= WARMUP_ROUNDS || period == 0) {
        usage(argv[0]);
        return 1;
    }

    if (HardwareUtils::pin_to_core(AppConfig::CORE_MAIN) != 0) {
        perror("pin main to coordinator core");
        return 1;
    }

    double tsc_ghz = HardwareUtils::calibrate_tsc_ghz();
    fprintf(stderr, "TSC frequency: %.3f GHz, threads on cores %d and %d\n", tsc_ghz, core_a, core_b);
    check_shared_l3(core_a, core_b);

    void *page = nullptr;
    volatile char *replica_a = nullptr, *replica_b = nullptr;
    if (!setup_replicas(channel_offset, &page, &replica_a, &replica_b)) return 1;

    printf("arm,n_samples,n_paired,tsc_ghz,min_cyc,p50_cyc,p90_cyc,p95_cyc,"
           "p99_cyc,p999_cyc,p9999_cyc,max_cyc,mean_cyc\n");

    Stats stats(tsc_ghz, raw_prefix);

    if (do_pair) {
        // The original benchmark's arm, unchanged: free-running threads, offline pairing
        AppConfig config;
        config.n_samples = n_rounds;
        config.raw_prefix = raw_prefix;
        Benchmark benchmark(config, tsc_ghz);
        benchmark.run_arm("pair", {replica_a, replica_b}, {core_a, core_b}, false, nullptr);
    }
    if (do_claim) {
        run_claim(stats, replica_a, replica_b, core_a, core_b, n_rounds, period, tsc_ghz);
    }

    munmap(page, AppConfig::SUPERPAGE_SIZE);
    return 0;
}
