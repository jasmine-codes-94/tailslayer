#include "single_call_analysis.hpp"
#include "../discovery/benchmark/app_config.hpp"

round_analysis analyze_rounds(const race_context& a, const race_context& b) {
    const uint64_t late_after = AppConfig::MAX_PAIR_GAP;
    int n_rounds = a.samples.size();

    round_analysis result;
    result.start_a.reserve(n_rounds);
    result.start_b.reserve(n_rounds);

    for (int k = 0; k < n_rounds; k++) {
        const round_sample& sample_a = a.samples[k];
        const round_sample& sample_b = b.samples[k];
        uint64_t deadline = a.deadlines[k]; // Both threads run on the same schedule

        result.start_a.push_back(sample_a.t0 - deadline);
        result.start_b.push_back(sample_b.t0 - deadline);

        bool late = result.start_a.back() >= late_after || result.start_b.back() >= late_after;
        if (late) result.late_rounds++;

        if (sample_a.won == sample_b.won) {
            result.not_exactly_once++;
            continue;
        }
        if (late) continue;

        const round_sample& winner = sample_a.won ? sample_a : sample_b;
        result.call_since_t0.push_back(winner.t_call - winner.t0);
        result.call_since_deadline.push_back(winner.t_call - deadline);
    }

    return result;
}
