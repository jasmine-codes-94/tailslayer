#include "setup_environment.hpp"
#include "../discovery/benchmark/app_config.hpp"
#include "../discovery/benchmark/hw_utils.hpp"

#include <cstdio>
#include <cstring>
#include <cinttypes>
#include <string>
#include <sys/mman.h>
#include <unistd.h>

// First line of a sysfs file without the newline, or "" if it can't be read
static std::string read_sysfs_line(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return "";
    char buf[256] = {};
    if (!fgets(buf, sizeof(buf), f)) buf[0] = '\0';
    fclose(f);
    buf[strcspn(buf, "\n")] = '\0';
    return buf;
}

/*
The CPUs that share this core's L3, as the kernel lists them (e.g. "0-7,16-23"),
or "" if it can't be read. Two cores share an L3 when their lists match.
*/
static std::string l3_shared_cpus(int core) {
    char path[128];
    for (int index = 0; ; ++index) {
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cache/index%d/level", core, index);
        std::string level = read_sysfs_line(path);
        if (level.empty()) return ""; // Ran out of cache levels without finding an L3
        if (level != "3") continue;

        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cache/index%d/shared_cpu_list", core, index);
        return read_sysfs_line(path);
    }
}

// Logical CPUs on the same physical core as this one (e.g. "11,27"), or "" if it can't be read
static std::string thread_siblings(int core) {
    char path[128];
    snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list", core);
    return read_sysfs_line(path);
}

// Whether this logical CPU exists on the machine (it has a directory in sysfs)
static bool cpu_exists(int core) {
    char path[128];
    snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d", core);
    return access(path, F_OK) == 0;
}

/*
All three cores (both workers and main's) must exist on this machine.
The claim's cost is a cache-line transfer between the two cores, so both must be:
- different cores: under chrt -f 99 the first thread would spin without yielding,
  so the second couldn't run until it finished
- separate physical cores: hyperthreads of one core share L1/L2, so the line would
  never leave the core and the claim would look unrealistically cheap
- not on the main thread's physical core: main spins while waiting for the workers
  to be ready, so a worker on its core could never run, and a worker on its sibling
  would share the core with it
- on the same L3 (on AMD: same CCX): otherwise the transfer crosses CCXs/CCDs and
  costs far more than it needs to
*/
static bool check_core_topology(int core_a, int core_b) {
    if (core_a == core_b) {
        fprintf(stderr, "ERROR: the two worker cores must be different (both are %d)\n", core_a);
        return false;
    }

    std::string present_cpus = read_sysfs_line("/sys/devices/system/cpu/present"); // e.g. "0-15"
    if (!cpu_exists(AppConfig::CORE_MAIN)) {
        fprintf(stderr, "ERROR: the main thread's core %d doesn't exist on this machine (CPUs: %s). "
                "Change AppConfig::CORE_MAIN in discovery/benchmark/app_config.hpp\n",
                AppConfig::CORE_MAIN, present_cpus.c_str());
        return false;
    }
    for (int core : {core_a, core_b}) {
        if (!cpu_exists(core)) {
            fprintf(stderr, "ERROR: worker core %d doesn't exist on this machine (CPUs: %s)\n",
                    core, present_cpus.c_str());
            return false;
        }
    }

    std::string siblings_a = thread_siblings(core_a);
    std::string siblings_b = thread_siblings(core_b);
    std::string siblings_main = thread_siblings(AppConfig::CORE_MAIN);
    if (siblings_a.empty() || siblings_b.empty() || siblings_main.empty()) {
        fprintf(stderr, "ERROR: couldn't check whether cores %d, %d and main's core %d are separate "
                "physical cores (no thread_siblings_list in sysfs)\n", core_a, core_b, AppConfig::CORE_MAIN);
        return false;
    } else if (siblings_a == siblings_b) {
        fprintf(stderr, "ERROR: cores %d and %d are hyperthreads of the same physical core (%s). "
                "Pick cores on separate physical cores\n", core_a, core_b, siblings_a.c_str());
        return false;
    } else if (siblings_a == siblings_main || siblings_b == siblings_main) {
        fprintf(stderr, "ERROR: a worker core (%d or %d) is on the same physical core as the main thread "
                "(core %d, siblings %s). Pick other cores\n", core_a, core_b, AppConfig::CORE_MAIN,
                siblings_main.c_str());
        return false;
    }

    std::string l3_a = l3_shared_cpus(core_a);
    std::string l3_b = l3_shared_cpus(core_b);
    if (l3_a.empty() || l3_b.empty()) {
        fprintf(stderr, "ERROR: couldn't check whether cores %d and %d share an L3 "
                "(no L3 shared_cpu_list in sysfs)\n", core_a, core_b);
        return false;
    } else if (l3_a != l3_b) {
        fprintf(stderr, "ERROR: cores %d and %d do NOT share an L3 (%s vs %s). "
                "Pick two cores from the same L3\n", core_a, core_b, l3_a.c_str(), l3_b.c_str());
        return false;
    }

    fprintf(stderr, "Cores %d and %d share an L3 (CPUs %s)\n", core_a, core_b, l3_a.c_str());
    return true;
}

static bool setup_memory(int channel_offset, int channel_bit, Environment& env) {
    env.replica_hugepage = mmap(nullptr, AppConfig::SUPERPAGE_SIZE, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | (30 << MAP_HUGE_SHIFT), -1, 0);
    if (env.replica_hugepage == MAP_FAILED) {
        perror("mmap 1GB hugepage (replicas)");
        env.replica_hugepage = nullptr;
        return false;
    }

    std::memset(env.replica_hugepage, 0x42, AppConfig::SUPERPAGE_SIZE);
    mlock(env.replica_hugepage, AppConfig::SUPERPAGE_SIZE);

    env.replica_a = static_cast<volatile char *>(env.replica_hugepage);
    env.replica_b = env.replica_a + channel_offset;

    // Informational only: needs root to read physical addresses
    volatile char *replicas[2] = { env.replica_a, env.replica_b };
    for (int i = 0; i < 2; ++i) {
        uint64_t phys = HardwareUtils::virt_to_phys(reinterpret_cast<uint64_t>(replicas[i]));
        if (phys == 0) {
            fprintf(stderr, "replica_%d: virt=%p (physical address unavailable, run as root)\n",
                    i, (void *)replicas[i]);
        } else {
            fprintf(stderr, "replica_%d: virt=%p phys=0x%" PRIx64 " channel=%d\n", i, (void *)replicas[i],
                    phys, HardwareUtils::compute_channel(phys, channel_bit));
        }
    }
    return true;
}

bool setup_environment(int core_a, int core_b, int channel_offset, int channel_bit, Environment& env) {
    if (!check_core_topology(core_a, core_b)) return false;

    if (HardwareUtils::pin_to_core(AppConfig::CORE_MAIN) != 0) {
        perror("pin main to coordinator core");
        return false;
    }
    fprintf(stderr, "Main thread pinned to core %d\n", AppConfig::CORE_MAIN);

    // Measured on the main core, after pinning
    env.tsc_ticks_per_ns = HardwareUtils::calibrate_tsc_ghz();
    fprintf(stderr, "TSC frequency: %.3f GHz\n", env.tsc_ticks_per_ns);

    return setup_memory(channel_offset, channel_bit, env);
}

void teardown_environment(Environment& env) {
    if (env.replica_hugepage) munmap(env.replica_hugepage, AppConfig::SUPERPAGE_SIZE);
    env = Environment{};
}
