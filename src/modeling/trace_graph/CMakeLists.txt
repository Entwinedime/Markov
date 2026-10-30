cmake_minimum_required(VERSION 3.20)
project(trace_graph LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(MARKOV_TRACE_GRAPH_DEBUG_DEFAULT OFF)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(MARKOV_TRACE_GRAPH_DEBUG_DEFAULT ON)
endif()

option(TRACE_GRAPH_DEBUG "Build trace_graph diagnostics and validation code" ${MARKOV_TRACE_GRAPH_DEBUG_DEFAULT})
find_package(nlohmann_json REQUIRED)

add_library(trace_graph_options INTERFACE)
target_compile_features(trace_graph_options INTERFACE cxx_std_23)
target_include_directories(trace_graph_options
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(trace_graph_options INTERFACE nlohmann_json::nlohmann_json)
if(TRACE_GRAPH_DEBUG)
    target_compile_definitions(trace_graph_options INTERFACE DEBUG)
endif()

add_library(trace_graph_core STATIC)
target_sources(trace_graph_core
    PRIVATE
        include/markov/trace_graph/json_scan.hpp
        src/core/trace_event.cpp
        src/core/dag_graph.cpp
        src/core/dag_index.cpp
        src/core/cpu_gap_observation.cpp
        src/core/client_requests.cpp
        src/core/dag_mutation.cpp
        src/core/dag_mutation_validation.cpp
        src/core/dag_builder.cpp
        src/core/dag_builder_correlation.cpp
        src/core/dag_builder_index.cpp
        src/core/dag_builder_normalization.cpp
        src/core/dag_builder_ordering.cpp
        src/core/dag_builder_sync.cpp
        src/core/logger.cpp
        src/core/execution_metrics.cpp
)
target_link_libraries(trace_graph_core PUBLIC trace_graph_options)

add_library(trace_graph_trace_adapters STATIC
    src/adapters/frameworks/sglang/trace_semantics.cpp
    src/adapters/runtimes/ascend/event_semantics.cpp
)
target_link_libraries(trace_graph_trace_adapters PUBLIC trace_graph_core)

add_library(trace_graph_io STATIC)
target_sources(trace_graph_io
    PRIVATE
        src/io/chrome_trace_io.cpp
        src/io/cpu_service_input.cpp
        src/io/trace_channel_join.cpp
        src/io/trace_manifest_input.cpp
)
target_link_libraries(trace_graph_io PUBLIC trace_graph_core trace_graph_trace_adapters)

add_library(trace_graph_frontend STATIC)
target_sources(trace_graph_frontend
    PRIVATE
        src/frontend/cost_package.cpp
        src/frontend/hicache_io_config.cpp
        src/frontend/hicache_service_config.cpp
        src/frontend/model_config.cpp
        src/frontend/trace_normalizer.cpp
)
target_link_libraries(trace_graph_frontend PUBLIC trace_graph_core trace_graph_trace_adapters trace_graph_cost)

add_library(trace_graph_simulation STATIC)
target_sources(trace_graph_simulation
    PRIVATE
        src/simulation/topological_simulator.cpp
        src/simulation/cpu_task_queues.cpp
)
target_link_libraries(trace_graph_simulation PUBLIC trace_graph_core)

add_library(trace_graph_source_dag STATIC)
target_sources(trace_graph_source_dag
    PRIVATE
        src/source_dag/build.cpp
        src/source_dag/replay.cpp
)
target_link_libraries(trace_graph_source_dag PUBLIC trace_graph_frontend trace_graph_io trace_graph_simulation)

add_library(trace_graph_source_index STATIC
    src/source_dag/index.cpp
    src/source_dag/stream_order.cpp
    src/source_dag/call_regions.cpp
)
target_link_libraries(trace_graph_source_index PUBLIC trace_graph_core trace_graph_simulation)

add_library(trace_graph_work STATIC
    src/structure/phase_shape.cpp
    src/structure/cpu_collective.cpp
)
target_link_libraries(trace_graph_work PUBLIC trace_graph_core)

add_library(trace_graph_cost STATIC
    src/cost/operator_model.cpp
    src/cost/host_operation.cpp
    src/cost/io_service.cpp
    src/cost/source_migration.cpp
)
target_link_libraries(trace_graph_cost PUBLIC trace_graph_core trace_graph_work)

add_library(trace_graph_sglang_work STATIC
    src/adapters/frameworks/sglang/attention_program.cpp
    src/adapters/frameworks/sglang/allocator_program.cpp
    src/adapters/frameworks/sglang/dense_program.cpp
)
target_link_libraries(trace_graph_sglang_work PUBLIC trace_graph_core trace_graph_work)

# Cache policy and target work consume typed inputs, never cost estimates.
add_library(trace_graph_hicache_rules STATIC
    src/modules/hicache/fact.cpp
    src/structure/hicache/phase.cpp
    src/structure/hicache/confirmation.cpp
    src/structure/hicache/prefetch.cpp
    src/structure/hicache/query.cpp
    src/structure/hicache/resources.cpp
    src/modules/hicache/router.cpp
    src/modules/hicache/policy.cpp
    src/modules/hicache/radix/token_radix_tree.cpp
    src/modules/hicache/radix/node_split_policy.cpp
    src/modules/hicache/runtime/token_store.cpp
    src/modules/hicache/runtime/target_pager.cpp
    src/modules/hicache/runtime/capacity_index.cpp
    src/modules/hicache/runtime/eviction_sequence.cpp
    src/modules/hicache/runtime/device_allocator.cpp
    src/modules/hicache/runtime/ref_ledger.cpp
    src/modules/hicache/runtime/target_control_clock.cpp
    src/modules/hicache/runtime/async_state.cpp
    src/modules/hicache/storage/storage_directory.cpp
    src/modules/hicache/model/state.cpp
    src/modules/hicache/model/prefetch_control.cpp
    src/modules/hicache/model/prefetch_execution.cpp
    src/modules/hicache/model/prefetch_check_execution.cpp
    src/modules/hicache/model/prefetch_queue_execution.cpp
    src/modules/hicache/model/prefetch_scheduler_body.cpp
    src/modules/hicache/model/prefetch_wait_execution.cpp
    src/modules/hicache/model/effect_decision.cpp
    src/modules/hicache/model/effect_ledger.cpp
    src/modules/hicache/model/request_model.cpp
    src/modules/hicache/model/decode_allocation_model.cpp
    src/modules/hicache/model/prefetch_model.cpp
    src/modules/hicache/model/host_storage_model.cpp
    src/modules/hicache/model/writeback_model.cpp
    src/modules/hicache/model/loadback_model.cpp
    src/modules/hicache/model/finalizer.cpp
)
target_link_libraries(trace_graph_hicache_rules PUBLIC trace_graph_core trace_graph_frontend trace_graph_work trace_graph_sglang_work crypto)

# Concrete source observations depend inward on graph and work contracts.
add_library(trace_graph_sglang_observations STATIC
    src/modules/hicache/phase_observation.cpp
    src/adapters/frameworks/sglang/source_calls.cpp
    src/adapters/frameworks/sglang/allocator_observations.cpp
    src/adapters/frameworks/sglang/dense_observations.cpp
    src/adapters/frameworks/sglang/collective_observations.cpp
    src/adapters/frameworks/sglang/models/qwen3_calls.cpp
    src/adapters/frameworks/sglang/models/qwen3_layout.cpp
    src/adapters/runtimes/ascend/operator_bindings.cpp
    src/source_dag/load_admission.cpp
    src/source_dag/decode_allocation.cpp
    src/modules/hicache/layer_waits.cpp
    src/adapters/frameworks/sglang/hicache_observations.cpp
    src/adapters/frameworks/sglang/allocator_preparation.cpp
)
target_link_libraries(trace_graph_sglang_observations PUBLIC trace_graph_source_index trace_graph_simulation trace_graph_hicache_rules trace_graph_sglang_work)

# Execution coordinates rules, observations, atomic mutations and cost binding.
add_library(trace_graph_hicache STATIC
    src/source_dag/observations.cpp
    src/source_dag/confirmation.cpp
    src/source_dag/collective_cost.cpp
    src/source_dag/phase_tensor_cost.cpp
    src/source_dag/tensor_cost.cpp
    src/source_dag/submission_cost.cpp
    src/source_dag/allocator_observations.cpp
    src/source_dag/prefetch.cpp
    src/source_dag/prefetch_query.cpp
    src/adapters/frameworks/sglang/execution_resources.cpp
    src/modules/hicache/cpu_collectives.cpp
    src/modules/hicache/cpu_collective_timing.cpp
    src/modules/hicache/prefetch_control_observation.cpp
    src/modules/hicache/prefetch_worker_observation.cpp
    src/modules/hicache/scope_observation.cpp
    src/modules/hicache/patch/cpu_collective_waits.cpp
    src/modules/hicache/patch/cpu_collective_composition.cpp
    src/modules/hicache/forward_cpu_service.cpp
    src/modules/hicache/host_cpu_service.cpp
    src/modules/hicache/runtime/write_capacity.cpp
    src/modules/hicache/runtime/load_capacity.cpp
    src/modules/hicache/runtime/load_admission.cpp
    src/modules/hicache/runtime/layer_calls.cpp
    src/modules/hicache/patch/prefetch_control_region.cpp
    src/modules/hicache/patch/io_resource_demand.cpp
    src/modules/hicache/runtime/io_completion.cpp
    src/modules/hicache/patch/io_resource_cost.cpp
    src/modules/hicache/patch/io_resource_model.cpp
    src/modules/hicache/patch/io_operation_ledger.cpp
    src/modules/hicache/patch/io_operation_record.cpp
    src/modules/hicache/patch/layer_wait_patch.cpp
    src/modules/hicache/patch/layer_wait_insertion.cpp
    src/modules/hicache/patch/layer_io.cpp
    src/modules/hicache/patch/attribution_common.cpp
    src/modules/hicache/patch/attribution_io.cpp
    src/modules/hicache/patch/attribution_commit.cpp
    src/modules/hicache/patch/attribution.cpp
    src/modules/hicache/model/apply_model.cpp
    src/modules/hicache/model/replay.cpp
    src/modules/hicache/model/source_input.cpp
    src/modules/hicache/runtime/source_preparation.cpp
    src/modules/hicache/execution_boundaries.cpp
    src/modules/hicache/control_points.cpp
    src/modules/hicache/runtime/prefetch_queries.cpp
    src/modules/hicache/runtime/prefetch_waits.cpp
    src/modules/hicache/runtime/queue_confirmations.cpp
    src/modules/hicache/runtime/write_confirmations.cpp
    src/modules/hicache/runtime/loads.cpp
    src/modules/hicache/runtime/host_program.cpp
    src/modules/hicache/runtime/host_compaction.cpp
    src/modules/hicache/runtime/host_cost_binding.cpp
    src/modules/hicache/runtime/tensor_program.cpp
    src/modules/hicache/runtime/allocator_calls.cpp
    src/modules/hicache/runtime/phase_calls.cpp
    src/modules/hicache/runtime/collective_services.cpp
    src/modules/hicache/runtime/collective_callers.cpp
    src/modules/hicache/runtime/load_submission.cpp
    src/modules/hicache/runtime/load_insertion.cpp
    src/modules/hicache/runtime/inactive_layer_calls.cpp
    src/modules/hicache/runtime/writes.cpp
    src/modules/hicache/runtime/storage_writes.cpp
    src/modules/hicache/runtime/write_calls.cpp
    src/modules/hicache/runtime/lifecycle_observation.cpp
    src/modules/hicache/runtime/lifecycle_calls.cpp
    src/modules/hicache/runtime/write_lifecycle.cpp
    src/modules/hicache/runtime/write_resources.cpp
    src/modules/hicache/runtime/control_calibration.cpp
    src/modules/hicache/runtime/write_frontier.cpp
    src/modules/hicache/runtime/write_host.cpp
    src/modules/hicache/runtime/write_expansion.cpp
    src/modules/hicache/runtime/host_removal.cpp
    src/modules/hicache/runtime/eviction_replacement.cpp
    src/modules/hicache/runtime/window.cpp
    src/modules/hicache/runtime/static_replay.cpp
)
target_link_libraries(trace_graph_hicache PUBLIC trace_graph_hicache_rules trace_graph_sglang_observations trace_graph_cost trace_graph_simulation)

# Reports are consumers of completed domain results, not algorithm dependencies.
add_library(trace_graph_reporting STATIC
    src/reporting/hicache_structure.cpp
)
target_link_libraries(trace_graph_reporting PUBLIC trace_graph_hicache)

add_library(trace_graph_modules STATIC)
target_sources(trace_graph_modules
    PRIVATE
        src/modules/node_scale/node_scale_module.cpp
        src/modules/hicache/hicache_module.cpp
)
target_link_libraries(trace_graph_modules
    PUBLIC
        trace_graph_core
        trace_graph_frontend
        trace_graph_hicache
)

add_library(trace_graph_cli_support STATIC)
target_sources(trace_graph_cli_support PRIVATE src/cli/file_output.cpp)
target_link_libraries(trace_graph_cli_support PUBLIC trace_graph_source_dag)
if(TRACE_GRAPH_DEBUG)
    target_sources(trace_graph_modules PRIVATE
        src/modules/hicache/dag_patch_module.cpp
        src/modules/hicache/observed_carrier.cpp
    )
    target_sources(trace_graph_hicache
        PRIVATE
            src/modules/hicache/diagnostics/static_phase.cpp
            src/modules/hicache/diagnostics/allocator_preparation.cpp
            src/modules/hicache/phase_carrier.cpp
            src/modules/hicache/patch/oracle_cost_replay.cpp
            src/modules/hicache/patch/rewrite_boundary.cpp
            src/modules/hicache/patch/rewrite_decision.cpp
            src/modules/hicache/patch/rewrite_dependencies.cpp
            src/modules/hicache/patch/rewrite_mutation.cpp
            src/modules/hicache/patch/rewrite_normalization.cpp
            src/modules/hicache/patch/rewrite_transaction.cpp
            src/modules/hicache/patch/boundary_validator.cpp
            src/modules/hicache/patch/applied_validator.cpp
    )

    add_library(trace_graph_diagnostics STATIC)
    target_sources(trace_graph_diagnostics
        PRIVATE
            src/modules/diagnostics/json_summary_writer.cpp
    )
    target_link_libraries(trace_graph_diagnostics PUBLIC trace_graph_modules trace_graph_reporting)

    target_sources(trace_graph_cli_support PRIVATE src/cli/debug_support_debug.cpp)
    target_link_libraries(trace_graph_cli_support PUBLIC trace_graph_diagnostics)
else()
    target_link_libraries(trace_graph_cli_support PUBLIC trace_graph_modules)
endif()

add_executable(trace_graph)
add_executable(hicache_calibration_export EXCLUDE_FROM_ALL src/cli/hicache_calibration_export.cpp)
add_dependencies(trace_graph hicache_calibration_export)
target_link_libraries(hicache_calibration_export PRIVATE trace_graph_cli_support trace_graph_modules trace_graph_simulation)
target_sources(trace_graph
    PRIVATE
        src/cli/options.cpp
        src/cli/module_pipeline.cpp
        src/cli/cpu_service_preparation.cpp
        src/cli/hicache_observations.cpp
        src/cli/run_summary.cpp
        src/cli/workflow.cpp
        src/cli/prediction_batch.cpp
        src/cli/main.cpp
)
target_link_libraries(trace_graph
    PRIVATE
        trace_graph_core
        trace_graph_io
        trace_graph_frontend
        trace_graph_simulation
        trace_graph_modules
        trace_graph_cli_support
        trace_graph_reporting
)

set_target_properties(trace_graph PROPERTIES
    OUTPUT_NAME trace_graph
)
