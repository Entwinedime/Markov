#pragma once

#include "markov/adapters/probes.hpp"

#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace markov::adapters::sglang {

struct AttentionDescriptor {
    std::optional<trace_graph::planning::ComputeOperation> operation;
    std::vector<trace_graph::planning::ComputeOperation> cache_writes;
    double page_tokens;
    double tensor_query_tokens;
    std::optional<std::vector<double>> query_tokens;
    trace_graph::source::Index layer;
    auto operator<=>(const AttentionDescriptor &) const = default;
};

// Probe observations stay at the framework boundary. They describe work and
// state; their enclosing wall duration is not a second CPU service charge.
class ProbeReader final : public adapters::ProbeReader {
public:
    using adapters::ProbeReader::ProbeReader;

private:
    void observe(const ProbeEvent & event) override;
    trace_graph::planning::PrefixId token_path(const nlohmann::json & dictionary);
    void observe_compute(const ProbeEvent & event);
    const AttentionDescriptor & attention_descriptor(const ProbeEvent & event, trace_graph::planning::Phase phase);
    std::map<std::string, trace_graph::source::Index> forward_frames_;
    struct EmbeddingWindow {
        trace_graph::source::Time lookup_end, reduction_begin;
    };
    std::map<std::string, EmbeddingWindow> embedding_windows_;
    std::map<std::string, trace_graph::planning::PrefixId> token_descriptors_;
    std::map<std::tuple<std::string, trace_graph::planning::Phase, trace_graph::source::Index>, AttentionDescriptor> attention_descriptors_;
};

} // namespace markov::adapters::sglang
