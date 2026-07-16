#include "model/exploration_metrics.hpp"

#include <iostream>

namespace model::diagnostics {
namespace {

ExplorationMetrics metrics;

struct MetricsReporter {
    ~MetricsReporter() {
        std::cout
            << "exploration_metrics:"
            << " naive_dfs_entries=" << metrics.naive_dfs_entries
            << " dpor_dfs_entries=" << metrics.dpor_dfs_entries
            << " collector_dfs_entries=" << metrics.collector_dfs_entries
            << " executed_steps=" << metrics.executed_steps
            << " branch_state_copies=" << metrics.branch_state_copies
            << " collector_state_copies=" << metrics.collector_state_copies
            << " state_container_elements="
            << metrics.branch_state_container_elements_copied
            << " state_map_nodes=" << metrics.branch_state_map_nodes_copied
            << " state_string_bytes=" << metrics.branch_state_string_bytes_copied
            << " state_clock_components="
            << metrics.branch_state_clock_components_copied
            << " state_schedule_steps=" << metrics.branch_state_schedule_steps_copied
            << " state_access_records=" << metrics.branch_state_access_records_copied
            << " state_buffer_entries=" << metrics.branch_state_buffer_entries_copied
            << " history_copies=" << metrics.state_history_copies
            << " collector_history_copies=" << metrics.collector_history_copies
            << " history_entries=" << metrics.state_history_entries_copied
            << " history_key_bytes=" << metrics.state_history_key_bytes_copied
            << " history_insertions=" << metrics.history_insertions
            << " history_restores=" << metrics.history_restores
            << " fingerprints=" << metrics.fingerprint_builds
            << " fingerprint_bytes=" << metrics.fingerprint_bytes
            << " clock_ticks=" << metrics.clock_ticks
            << " clock_joins=" << metrics.clock_joins
            << " clock_join_components=" << metrics.clock_join_components
            << " clock_comparisons=" << metrics.clock_comparisons
            << " clock_compare_components=" << metrics.clock_compare_components
            << " clock_map_insertions=" << metrics.clock_map_insertions
            << " enabled_collections=" << metrics.enabled_collections
            << " enabled_thread_probes=" << metrics.enabled_thread_probes
            << " enabled_steps=" << metrics.enabled_steps_emitted
            << " transition_maps=" << metrics.dpor_enabled_transition_maps
            << " transition_entries=" << metrics.dpor_enabled_transition_entries
            << " effective_actions=" << metrics.effective_actions_materialized
            << " action_string_bytes=" << metrics.action_string_bytes_materialized
            << " dpor_node_snapshots=" << metrics.dpor_node_snapshots
            << " dpor_node_map_nodes=" << metrics.dpor_node_map_nodes_copied
            << " dpor_node_sequence_elements="
            << metrics.dpor_node_sequence_elements_copied
            << " dpor_node_clock_components="
            << metrics.dpor_node_clock_components_copied
            << " dpor_node_buffer_entries=" << metrics.dpor_node_buffer_entries_copied
            << " independence_checks=" << metrics.dpor_independence_checks
            << " prefix_entries_scanned=" << metrics.dpor_prefix_entries_scanned
            << " schedule_pushes=" << metrics.schedule_pushes
            << " report_schedule_copies=" << metrics.report_schedule_copies
            << " report_schedule_steps=" << metrics.report_schedule_steps_copied
            << " replay_calls=" << metrics.replay_calls
            << " replay_steps=" << metrics.replay_steps
            << " effective_replay_calls=" << metrics.effective_replay_calls
            << " minimization_calls=" << metrics.minimization_calls
            << " minimization_candidate_replays="
            << metrics.minimization_candidate_replays
            << " minimization_schedule_steps="
            << metrics.minimization_schedule_steps_copied
            << '\n';
    }
};

MetricsReporter reporter;

} // namespace

void reset_exploration_metrics() { metrics = ExplorationMetrics{}; }

ExplorationMetrics exploration_metrics() { return metrics; }

namespace detail {

ExplorationMetrics& mutable_exploration_metrics() { return metrics; }

} // namespace detail
} // namespace model::diagnostics
