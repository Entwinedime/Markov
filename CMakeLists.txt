cmake_minimum_required(VERSION 3.20)
project(markov LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(nlohmann_json 3.11 REQUIRED)
find_package(ZLIB REQUIRED)

add_library(markov_core
    src/markov/src/contracts/control.cpp
    src/markov/src/contracts/json_input.cpp
    src/markov/src/contracts/service_work.cpp
    src/markov/src/contracts/cpu_service.cpp
    src/markov/src/controller/lightweight_controller.cpp
    src/markov/src/execution/execution_graph.cpp
    src/markov/src/execution/cpu_services.cpp
    src/markov/src/execution/simulation.cpp
    src/markov/src/execution/workspace_state.cpp
    src/markov/src/graph/cpu_service_partition.cpp
    src/markov/src/graph/kernel_shapes.cpp
    src/markov/src/graph/source_graph.cpp
    src/markov/src/performance/node_durations.cpp
    src/markov/src/performance/node_costs.cpp
    src/markov/src/performance/cost_query.cpp
    src/markov/src/performance/cost_requirements.cpp
    src/markov/src/performance/cost_statistics.cpp
    src/markov/src/performance/kernel_cost_model.cpp
    src/markov/src/performance/communication_cost_model.cpp
    src/markov/src/performance/transfer_cost_model.cpp
    src/markov/src/production/block_binding.cpp
    src/markov/src/production/block_extractor.cpp
    src/markov/src/production/source_call.cpp
    src/markov/src/production/source_subgraph.cpp
    src/markov/src/templates/attention_parameters.cpp
    src/markov/src/templates/batch_parameters.cpp
    src/markov/src/templates/collective_replacement.cpp
    src/markov/src/templates/communication_work.cpp
    src/markov/src/templates/kv_parameters.cpp
    src/markov/src/templates/kv_workload.cpp
    src/markov/src/templates/model_parameters.cpp
    src/markov/src/templates/publication_parameters.cpp
    src/markov/src/templates/region_sequence.cpp
    src/markov/src/templates/region_structure.cpp
    src/markov/src/templates/request_pool_parameters.cpp
    src/markov/src/templates/template_assembly.cpp
    src/markov/src/templates/template_asset.cpp
    src/markov/src/templates/template_topology.cpp
    src/markov/src/templates/template_definition.cpp
    src/markov/src/templates/template_bank.cpp
    src/markov/src/templates/tensor_parameters.cpp
    src/markov/src/templates/transfer_parameters.cpp
    src/markov/src/templates/workspace_parameters.cpp
)
target_include_directories(markov_core PUBLIC src/markov/include)
target_link_libraries(markov_core PUBLIC nlohmann_json::nlohmann_json ZLIB::ZLIB)
target_compile_options(markov_core PRIVATE -Wall -Wextra -Wpedantic)

add_library(markov_adapters
    src/markov/src/adapters/template_adapter.cpp
    src/markov/src/adapters/frameworks/ktransformers/collection_adapter.cpp
    src/markov/src/adapters/frameworks/sglang/collection_adapter.cpp
    src/markov/src/adapters/frameworks/sglang/configuration.cpp
    src/markov/src/adapters/frameworks/sglang/batch_layout.cpp
    src/markov/src/adapters/frameworks/sglang/bindings.cpp
    src/markov/src/adapters/frameworks/sglang/source_ports.cpp
    src/markov/src/adapters/frameworks/sglang/template_adapter.cpp
    src/markov/src/adapters/frameworks/sglang/copy_layout.cpp
    src/markov/src/adapters/frameworks/sglang/models/attention_kernel_layout.cpp
    src/markov/src/adapters/frameworks/sglang/models/model_kernel_layout.cpp
    src/markov/src/adapters/frameworks/sglang/models/qwen3_regions.cpp
    src/markov/src/adapters/frameworks/sglang/paged_kv_layout.cpp
    src/markov/src/adapters/frameworks/sglang/request_pool_layout.cpp
    src/markov/src/adapters/frameworks/sglang/sampling_recipes.cpp
    src/markov/src/adapters/frameworks/sglang/template_bindings.cpp
    src/markov/src/adapters/frameworks/sglang/template_production.cpp
    src/markov/src/adapters/runtimes/ascend/hccl_mesh_layout.cpp
    src/markov/src/adapters/runtimes/ascend/source_extractor.cpp
    src/markov/src/adapters/runtimes/ascend/source_cpu.cpp
    src/markov/src/adapters/runtimes/ascend/source_events.cpp
    src/markov/src/adapters/runtimes/ascend/source_transfers.cpp
    src/markov/src/adapters/runtimes/ascend/workspace_bindings.cpp
)
target_include_directories(markov_adapters PUBLIC src/markov/include)
target_link_libraries(markov_adapters PUBLIC markov_core)
target_compile_options(markov_adapters PRIVATE -Wall -Wextra -Wpedantic)

add_library(markov_workflow
    src/markov/src/workflow/project.cpp
    src/markov/src/workflow/extraction_workflow.cpp
    src/markov/src/workflow/cost_fitting_workflow.cpp
    src/markov/src/workflow/plan.cpp
    src/markov/src/workflow/project_reader.cpp
    src/markov/src/workflow/prediction_workflow.cpp
    src/markov/src/workflow/cpu_sampling_catalog.cpp
    src/markov/src/workflow/performance_sampling_plan.cpp
)
target_include_directories(markov_workflow PUBLIC src/markov/include)
target_link_libraries(markov_workflow PUBLIC markov_adapters)
target_compile_options(markov_workflow PRIVATE -Wall -Wextra -Wpedantic)

add_executable(markov src/markov/src/cli/main.cpp)
target_link_libraries(markov PRIVATE markov_workflow)
target_compile_options(markov PRIVATE -Wall -Wextra -Wpedantic)
