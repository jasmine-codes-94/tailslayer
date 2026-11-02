#ifndef APP_CONFIG_HPP
#define APP_CONFIG_HPP

#include <array>
#include <cstdint>
#include <string_view>

struct AppConfig {
    // Very hardware specific. Hardcoded defaults that change
    static inline constexpr int CORE_MEAS_A = 2;
    static inline constexpr int CORE_MEAS_B = 3;
    static inline constexpr int CORE_MAIN = 1;
    static inline constexpr int DEFAULT_CHANNEL_OFFSET = 256; // The offset between the replicas to end up on different channels
    static inline constexpr int DEFAULT_CHANNEL_BIT = 8; // Bit in the physical memory address that says which channel the address belongs to
    static inline constexpr int DEFAULT_NUM_CHANNELS = 2;
    static inline constexpr std::array<int, 10> STRESS_CORES = {{8, 9, 10, 13, 15, 24, 25, 26, 29, 31}};
    static inline constexpr uint64_t SUPERPAGE_SIZE = (1ULL << 30); // 1GB hugepage

    // Default benchmark configurations
    static inline constexpr int DEFAULT_SAMPLES = 5000000;
    static inline constexpr int DEFAULT_STRESS = 4;
    static inline constexpr int WARMUP_ITERS = 5000;
    static inline constexpr int MAX_PAIR_GAP = 400;
    static inline constexpr int MAX_STRESS = 16;

    // What kind of run we want to perform
    bool do_all = false;
    bool do_single_quiet = false;
    bool do_hedged_quiet = false;
    bool do_single_stress = false;
    bool do_hedged_stress = false;

    // Configuration for the run
    int n_samples = DEFAULT_SAMPLES;
    int n_stress = DEFAULT_STRESS;

    // Probably change, but just set them to the most likely defaults
    int core_a = CORE_MEAS_A;
    int core_b = CORE_MEAS_B;
    int channel_bit = DEFAULT_CHANNEL_BIT;
    int channel_offset = DEFAULT_CHANNEL_OFFSET;
    int n_channels = DEFAULT_NUM_CHANNELS;

    std::string_view raw_prefix = "";

    static AppConfig parse_cli(int argc, char* argv[]);
    static void usage(const char *prog);
};

#endif // APP_CONFIG_HPP