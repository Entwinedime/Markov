#include "markov/adapters/sglang/observations.hpp"

#include <stdexcept>
#include <utility>
#include <vector>

namespace markov::adapters::sglang {

using namespace trace_graph::source;

void ProbeReader::observe(const ProbeEvent & event) {
    const auto & name = event.name;
    const auto & args = event.args;
    const auto & process = event.process;
    if (name == "hicache_cache_extend_input_start") forward_phases_[process] = "prefill";
    if (name == "runtime.hicache.decode_allocation") forward_phases_[process] = "decode";

    if (name == "hicache_cache_lookup_input_end" || name == "hicache_cache_finished_req_lifecycle_commit_end") {
        const auto path = token_path(args.at("token_dictionary"));
        const auto role = name == "hicache_cache_lookup_input_end" ? TokenRole::LookupInput : TokenRole::Committed;
        output_.observe_tokens(args.at("request_id").get<std::string>(), role, path, event.record);
    }
    if (name == "hicache_cache_extend_input_start") {
        const auto & dictionaries = args.at("token_dictionaries");
        const auto & identities = args.at("request_ids");
        for (std::size_t i = 0; i < identities.size(); ++i) {
            const auto path = token_path(dictionaries.at(i));
            output_.observe_tokens(identities[i].get<std::string>(), TokenRole::Prompt, path, event.record);
        }
    }

    if (name == "runtime.phase.linear") {
        const auto resource = output_.resource(ResourceKind::CpuThread, "cpu:" + process + ":" + event.thread);
        observe_linear(args, process, resource, event.begin_ns, event.end_ns, event.record);
    }
}

trace_graph::planning::PrefixId ProbeReader::token_path(const nlohmann::json & dictionary) {
    const auto identity = dictionary.at("token_path_id").get<std::string>();
    if (!dictionary.contains("token_ids")) return token_descriptors_.at(identity);

    // Native probes declare each payload once, then refer to its dictionary ID.
    // Resolve that protocol here; target identity uses the actual token ancestry.
    const auto tokens = dictionary.at("token_ids").get<std::vector<std::int64_t>>();
    const auto path = output_.token_paths.intern(tokens);
    const auto [entry, inserted] = token_descriptors_.try_emplace(identity, path);
    if (!inserted && entry->second != path) throw std::runtime_error("Source token dictionary reuses an ID for different content");
    return path;
}

void ProbeReader::observe_linear(const nlohmann::json & args, const std::string & process, Index resource, Time begin, Time end, RecordRef record) {
    if (args.at("status") != "returned") return;
    const auto phase = forward_phases_.find(process);
    if (phase == forward_phases_.end()) {
        ++output_.counts["linear_work_without_forward_context"];
        return;
    }

    const auto input = args.at("input_shape").get<std::vector<double>>();
    const auto weight = args.at("weight_shape").get<std::vector<double>>();
    const auto input_stride = args.at("input_stride").get<std::vector<double>>();
    const auto weight_stride = args.at("weight_stride").get<std::vector<double>>();
    if (input.size() != 2 || weight.size() != 2 || input_stride.size() != 2 || weight_stride.size() != 2) {
        ++output_.counts["linear_work_unsupported_geometry"];
        return;
    }
    const auto m = input[0], k = input[1], n = weight[0];
    auto implementation = "torch.linear:" + args.at("input_dtype").get<std::string>();
    implementation += args.at("bias_shape").is_null() ? ":no_bias" : ":bias";
    if (args.at("input_dtype") != args.at("weight_dtype")) implementation += ":weight=" + args.at("weight_dtype").get<std::string>();

    // The probe supplies geometry and provenance, not a device measurement.
    // The independent provider estimates service; this CPU envelope is not charged.
    trace_graph::planning::Work work{
        { phase->second + ".linear", std::move(implementation), "device_service" },
        { { "m", m },
         { "n", n },
         { "k", k },
         { "multiply_accumulate_count", m * n * k },
         { "input_elements", m * k },
         { "output_elements", m * n },
         { "weight_elements", n * k },
         { "input_stride_0", input_stride[0] },
         { "input_stride_1", input_stride[1] },
         { "weight_stride_0", weight_stride[0] },
         { "weight_stride_1", weight_stride[1] } }
    };
    const auto description = output_.describe_work(std::move(work));
    output_.work_observations.push_back({ description, resource, begin, end, record });
}

} // namespace markov::adapters::sglang
