#ifndef SETUP_ENVIRONMENT_HPP
#define SETUP_ENVIRONMENT_HPP

/*
 * setup_environment — Everything that has to be in place before a race: valid worker
 * cores, main pinned, the TSC frequency, and the hugepage holding the two replicas.
 */

struct Environment {
    double tsc_ticks_per_ns = 0.0;         // TSC frequency, for converting TSC ticks to nanoseconds
    void *replica_hugepage = nullptr;      // The 1GB hugepage both replicas live in
    volatile char *replica_a = nullptr;    // Start of the hugepage
    volatile char *replica_b = nullptr;    // channel_offset bytes later, on the other channel
};

/*
In order, stopping (after printing why) at the first failure:
1. Checks that both worker cores and main's core exist, and that core_a and core_b are
   separate physical cores that share an L3, neither on the main thread's physical core,
   verified from sysfs
2. Pins the main thread to AppConfig::CORE_MAIN
3. Calibrates the TSC frequency
4. Maps a 1GB hugepage, places two replicas channel_offset bytes apart, and prints which
   channel each is on, taking bit channel_bit of the physical address as the channel
channel_offset and channel_bit are machine-specific (worked out per architecture), so the
caller provides them and they aren't checked here.
*/
bool setup_environment(int core_a, int core_b, int channel_offset, int channel_bit, Environment& env);

// Unmaps the hugepage
void teardown_environment(Environment& env);

#endif // SETUP_ENVIRONMENT_HPP
