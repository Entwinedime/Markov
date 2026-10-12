#include "markov/adapters/runtimes/ascend/workspace_bindings.hpp"

#include "markov/templates/templates.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>

namespace markov {

void AscendWorkspaceBindings::bind(TemplateSource & source, const nlohmann::json & facts, const nlohmann::json & graph) const {
    using PoolKey = std::tuple<std::uint32_t, std::uint64_t, std::int64_t, std::uint64_t>;
    std::map<PoolKey, std::uint64_t> initial;
    std::set<std::pair<std::uint32_t, std::uint64_t>> workers;
    const std::map<std::string, WorkspaceBranch> branches{
        {    "reuse",    WorkspaceBranch::Reuse },
        { "allocate", WorkspaceBranch::Allocate },
        {     "grow",     WorkspaceBranch::Grow },
        {  "release",  WorkspaceBranch::Release }
    };
    const auto windows = facts.find("workspace_states");
    if (windows == facts.end() || windows->empty()) {
        source.workspace_binding_gap = "workspace_window_not_observed";
        return;
    }
    for (const auto & window : *windows) {
        if (window.at("initial").is_null() || window.at("final").is_null() || window.at("probe_installation").is_null()) {
            source.workspace_binding_gap = "workspace_window_not_observed";
            return;
        }
        workers.emplace(window.at("rank"), window.at("pid"));
        for (const auto & pool : window.at("initial"))
            initial.emplace(PoolKey{ window.at("rank"), window.at("pid"), pool.at("device"), pool.at("stream_handle") }, pool.at("capacity_bytes"));
    }

    std::map<std::size_t, TemplateDefinition *> definitions;
    for (auto & [key, members] : source.definitions)
        for (auto & definition : members) definitions.emplace(definition.source_call, &definition);

    const auto & spans = graph.at("spans");
    const auto & boundaries = graph.at("boundaries");
    std::map<std::size_t, std::size_t> interval_spans;
    for (std::size_t index = 0; index < spans.size(); ++index)
        if (!spans[index].at("source_interval").is_null()) interval_spans.emplace(spans[index].at("source_interval"), index);

    std::map<PoolKey, std::size_t> pools;
    const auto & records = facts.at("native_api");
    for (std::size_t api = 0; api < records.size(); ++api) {
        const auto & record = records[api];
        const auto transition = record.find("workspace");
        if (transition == record.end()) continue;
        if (!workers.contains({ record.at("rank"), record.at("pid") })) {
            source.workspace_binding_gap = "workspace_window_not_observed";
            return;
        }
        const auto allocation = record.find("workspace_allocation");
        if (allocation == record.end() || record.at("source_interval").is_null() || record.at("stream_id").is_null()) {
            source.workspace_binding_gap = "workspace_allocation_not_qualified";
            return;
        }
        const auto span_id = interval_spans.at(record.at("source_interval"));
        const auto & span = spans[span_id];
        if (span.at("owner_call").is_null() || span.at("scope_call").is_null() || !definitions.contains(span.at("owner_call"))) {
            source.workspace_binding_gap = "workspace_serving_owner_unresolved";
            return;
        }
        auto & definition = *definitions.at(span.at("owner_call"));
        const auto & begin = boundaries.at(span.at("begin").get<std::size_t>());
        const auto & end = boundaries.at(span.at("end").get<std::size_t>());
        std::optional<std::pair<std::size_t, std::size_t>> owner;
        for (std::size_t operation = 0; operation < definition.operations.size(); ++operation) {
            const auto & workers = definition.operations[operation].workers;
            for (std::size_t worker = 0; worker < workers.size(); ++worker) {
                const auto & candidate = spans.at(workers[worker].source_span);
                const auto & entry = boundaries.at(candidate.at("begin").get<std::size_t>());
                const auto & exit = boundaries.at(candidate.at("end").get<std::size_t>());
                // Queue flows already bind this worker to its host operation.
                // Same scope, lane and containment locate the allocator inside it.
                if (candidate.at("scope_call") != span.at("scope_call") || entry.at("lane") != begin.at("lane")
                    || entry.at("observed_time_ns") > begin.at("observed_time_ns") || exit.at("observed_time_ns") < end.at("observed_time_ns"))
                    continue;
                if (owner) {
                    source.workspace_binding_gap = "workspace_operation_ambiguous";
                    return;
                }
                owner = { operation, worker };
            }
        }
        if (!owner) {
            source.workspace_binding_gap = "workspace_operation_unresolved";
            return;
        }

        const PoolKey key{ record.at("rank"), record.at("pid"), transition->at("device"), record.at("stream_handle") };
        auto pool = pools.find(key);
        if (pool == pools.end()) {
            std::optional<TemplateResource> resource;
            for (const auto & lane : definition.lanes)
                if (lane.source.domain == SourceDomain::DeviceStream && lane.source.rank == record.at("rank")
                    && lane.source.native_id == record.at("stream_id"))
                    resource = lane.resource();
            if (!resource) {
                source.workspace_binding_gap = "workspace_resource_unresolved";
                return;
            }
            // A stream absent from a complete initial snapshot starts empty.
            // Failure to observe the snapshot was handled above, not defaulted.
            const auto state = initial.find(key);
            pool = pools.emplace(key, source.workspace_pools.size()).first;
            // The normalizer qualifies the pinned cached allocator implementation.
            // Export its allocation geometry, not a fitted capacity or duration.
            source.workspace_pools.push_back({ *resource, state == initial.end() ? 0 : state->second, 32, 2 * 1'024 * 1'024 });
        }
        if (source.workspace_pools[pool->second].resource.native_id != record.at("stream_id")) {
            source.workspace_binding_gap = "workspace_stream_identity_changed";
            return;
        }
        std::optional<std::size_t> local_begin, local_end;
        for (std::size_t index = 0; index < definition.boundary_observations.size(); ++index) {
            const auto boundary = definition.boundary_observations[index].source_boundary;
            if (boundary == span.at("begin")) local_begin = index;
            if (boundary == span.at("end")) local_end = index;
        }
        if (!local_begin || !local_end) {
            source.workspace_binding_gap = "workspace_operation_boundaries_missing";
            return;
        }
        definition.workspace_uses.push_back({ pool->second,
                                              owner->first,
                                              owner->second,
                                              api,
                                              span_id,
                                              *local_begin,
                                              *local_end,
                                              transition->at("requested_bytes"),
                                              transition->at("capacity_before_bytes"),
                                              transition->at("capacity_after_bytes"),
                                              branches.at(allocation->at("branch").get<std::string>()),
                                              allocation->at("requires_device_synchronization") });

        // Allocator replacement uses the same external interface qualification
        // as other regions, including retained work and recorded device waits.
        const auto region = definition.workspace_region(definition.workspace_uses.back());
        const auto ports = definition.region_ports(region);
        if (!ports) {
            source.workspace_binding_gap = ports.error();
            return;
        }
        const auto resource = definition.lanes[definition.graph.boundary(*local_begin).lane].resource();
        if (ports->size() != 1 || ports->front().resource != resource || ports->front().entry != *local_begin || ports->front().exit != *local_end) {
            source.workspace_binding_gap = "workspace_region_interface_not_qualified";
            return;
        }
    }
    for (const auto & [key, capacity] : initial) {
        if (!pools.contains(key)) {
            source.workspace_binding_gap = "workspace_initial_resource_unresolved";
            return;
        }
    }
}

void AscendWorkspaceBindings::bind_demands(TemplateSource & source, const nlohmann::json & facts, const nlohmann::json & environment) const {
    const auto & atb = environment.at("runtime").at("atb");
    std::optional<std::string> environment_gap;
    if (atb.is_null() || atb.at("version_info").is_null()) environment_gap = "workspace_atb_version_not_observed";
    else if (atb.at("version_info").get<std::string>().find("8bf8f4054d644f14d3924a7f0b4d0467ef5cfa52") == std::string::npos)
        environment_gap = "workspace_atb_implementation_not_supported";

    std::map<std::tuple<std::uint32_t, std::uint64_t, std::int64_t>, std::uint32_t> cores;
    for (const auto & device : facts.at("devices"))
        cores.emplace(std::tuple{ device.at("rank").get<std::uint32_t>(), device.at("pid").get<std::uint64_t>(), device.at("device").get<std::int64_t>() },
                      device.at("cube_cores"));

    // Native ATB tiling constants and branches, never measurements or fitted coefficients.
    // PagedAttention: SplitCoreBNND and eight scratch buffers; Runner aligns to 512 bytes.
    for (auto & [key, definitions] : source.definitions) {
        for (auto & definition : definitions) {
            BatchInput input{};
            for (const auto & request : definition.source_requests)
                input.requests.push_back({ 0, request.input_tokens, request.context_tokens, request.generated_tokens, false, 0, {} });

            for (auto & use : definition.workspace_uses) {
                if (environment_gap) {
                    use.demand = std::unexpected(*environment_gap);
                    continue;
                }
                const auto & record = facts.at("native_api").at(use.source_api);
                const auto capacity = cores.find({ record.at("rank"), record.at("pid"), record.at("workspace").at("device") });
                if (capacity == cores.end()) {
                    use.demand = std::unexpected("workspace_device_geometry_not_observed");
                    continue;
                }
                const auto units = capacity->second;
                const auto & operation = definition.operations[use.operation];
                if (operation.device_spans.size() != 1) {
                    use.demand = std::unexpected("workspace_operation_device_work_not_qualified");
                    continue;
                }
                const auto span = operation.device_spans.front();
                const auto layout = definition.attention_kernel_layouts.find(span);
                if (layout == definition.attention_kernel_layouts.end()) {
                    use.demand = std::unexpected("workspace_attention_parameters_not_qualified");
                    continue;
                }
                const auto & parameters = layout->second.rule->geometry();
                const auto heads = static_cast<std::uint32_t>(parameters.query_heads);
                const auto dimension = static_cast<std::uint32_t>(parameters.head_dim);
                const auto & kernel = source.kernels.at(span);
                std::optional<WorkspaceDemandLayout> demand;
                if (kernel.operation == "UnpadFlashAttentionBF16PrefixCacheNdKernel") {
                    if (heads < units) {
                        use.demand = std::unexpected("workspace_prefill_partition_not_supported");
                        continue;
                    }
                    // Every nonempty request contributes at least one tile per head.
                    // Prefix-cache encoder cannot enter the separate encoder splitm path.
                    demand.emplace(units, 9ULL * 32'768 * 8 * sizeof(float), 512, std::nullopt, std::vector<WorkspaceScaledBuffer>{});
                }
                else if (kernel.operation == "PagedAttentionMaskNdKernel") {
                    if (dimension > 256) {
                        use.demand = std::unexpected("workspace_decode_head_dimension_not_supported");
                        continue;
                    }
                    WorkspacePartition partition{ heads, 32, static_cast<std::uint32_t>((units * 4ULL + 5ULL * heads - 1) / (5ULL * heads)), units * 256, {} };
                    // SplitCoreBNND explicitly selects eight heads per group for 16 members / 32 heads.
                    if (heads == 32) partition.group_size_by_members.emplace(16, 8);
                    const auto linear_bytes = 65'536ULL * 18 + 2ULL * 2 * 256 * heads * dimension * sizeof(std::uint16_t);
                    demand.emplace(units,
                                   linear_bytes,
                                   512,
                                   partition,
                                   std::vector<WorkspaceScaledBuffer>{
                                       { 0.8F, heads, dimension * static_cast<std::uint32_t>(sizeof(float)) },
                                       { 0.8F, heads,                                         sizeof(float) }
                    });
                }
                if (!demand) {
                    use.demand = std::unexpected("workspace_kernel_operation_not_supported");
                    continue;
                }
                const auto observed = demand->evaluate(input);
                if (!observed) {
                    use.demand = std::unexpected(observed.error());
                    continue;
                }
                if (observed->bytes != use.requested_bytes) {
                    use.demand = std::unexpected("workspace_observed_demand_mismatch");
                    continue;
                }
                const auto [rule, inserted] = source.workspace_rules.insert(std::move(*demand));
                use.demand = &*rule;
            }
        }
    }
}

} // namespace markov
