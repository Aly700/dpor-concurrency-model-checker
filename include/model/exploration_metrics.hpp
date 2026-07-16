#pragma once

#include <cstdint>

namespace model::diagnostics {

struct ExplorationMetrics {
    std::uint64_t naive_dfs_entries{0};
    std::uint64_t dpor_dfs_entries{0};
    std::uint64_t collector_dfs_entries{0};
    std::uint64_t executed_steps{0};
    std::uint64_t branch_state_copies{0};
    std::uint64_t collector_state_copies{0};
    std::uint64_t branch_state_container_elements_copied{0};
    std::uint64_t branch_state_map_nodes_copied{0};
    std::uint64_t branch_state_string_bytes_copied{0};
    std::uint64_t branch_state_clock_components_copied{0};
    std::uint64_t branch_state_schedule_steps_copied{0};
    std::uint64_t branch_state_access_records_copied{0};
    std::uint64_t branch_state_buffer_entries_copied{0};
    std::uint64_t state_history_copies{0};
    std::uint64_t collector_history_copies{0};
    std::uint64_t state_history_entries_copied{0};
    std::uint64_t state_history_key_bytes_copied{0};
    std::uint64_t history_insertions{0};
    std::uint64_t history_restores{0};
    std::uint64_t fingerprint_builds{0};
    std::uint64_t fingerprint_bytes{0};
    std::uint64_t clock_ticks{0};
    std::uint64_t clock_joins{0};
    std::uint64_t clock_join_components{0};
    std::uint64_t clock_comparisons{0};
    std::uint64_t clock_compare_components{0};
    std::uint64_t clock_map_insertions{0};
    std::uint64_t enabled_collections{0};
    std::uint64_t enabled_thread_probes{0};
    std::uint64_t enabled_steps_emitted{0};
    std::uint64_t dpor_enabled_transition_maps{0};
    std::uint64_t dpor_enabled_transition_entries{0};
    std::uint64_t effective_actions_materialized{0};
    std::uint64_t action_string_bytes_materialized{0};
    std::uint64_t dpor_node_snapshots{0};
    std::uint64_t dpor_node_map_nodes_copied{0};
    std::uint64_t dpor_node_sequence_elements_copied{0};
    std::uint64_t dpor_node_clock_components_copied{0};
    std::uint64_t dpor_node_buffer_entries_copied{0};
    std::uint64_t dpor_independence_checks{0};
    std::uint64_t dpor_prefix_entries_scanned{0};
    std::uint64_t schedule_pushes{0};
    std::uint64_t report_schedule_copies{0};
    std::uint64_t report_schedule_steps_copied{0};
    std::uint64_t replay_calls{0};
    std::uint64_t replay_steps{0};
    std::uint64_t effective_replay_calls{0};
    std::uint64_t minimization_calls{0};
    std::uint64_t minimization_candidate_replays{0};
    std::uint64_t minimization_schedule_steps_copied{0};

    bool operator==(const ExplorationMetrics&) const = default;
};

void reset_exploration_metrics();
[[nodiscard]] ExplorationMetrics exploration_metrics();

namespace detail {

ExplorationMetrics& mutable_exploration_metrics();

} // namespace detail
} // namespace model::diagnostics
