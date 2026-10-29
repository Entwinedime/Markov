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
    ProbeReader(trace_graph::source::Observations & output, std::optional<bool> deterministic_sampling)
        : adapters::ProbeReader(output),
          deterministic_sampling_(deterministic_sampling) {}

private:
    void observe(const ProbeEvent & event) override;
    void finish() override;
    trace_graph::planning::PrefixId token_path(const nlohmann::json & dictionary);
    void observe_compute(const ProbeEvent & event);
    void observe_collective(const ProbeEvent & event);
    void observe_result(const ProbeEvent & event);
    void observe_control(const ProbeEvent & event);
    std::optional<bool> deterministic_sampling_;
    const AttentionDescriptor & attention_descriptor(const ProbeEvent & event, trace_graph::planning::Phase phase);
    std::map<std::string, trace_graph::source::Index> forward_frames_;
    struct Lookup {
        trace_graph::source::Index request;
        trace_graph::source::ObservedRegion region;
    };
    std::vector<Lookup> lookups_;
    using BatchIdentity = std::pair<std::string, trace_graph::source::Index>;
    std::map<BatchIdentity, trace_graph::source::Index> batch_results_;
    std::map<BatchIdentity, trace_graph::source::ResultCopy> result_copies_;
    std::map<BatchIdentity, std::vector<trace_graph::source::ResultSend>> result_sends_;
    std::map<trace_graph::source::Index, std::vector<trace_graph::source::Index>> pending_controls_;
    std::map<std::string, trace_graph::source::ObservedCallSequence> pending_decode_allocations_;
    struct InputWindow {
        trace_graph::source::ObservedRegion allocator;
        std::optional<trace_graph::source::ObservedRegion> table_write, forward;
    };
    std::map<std::string, InputWindow> input_windows_;
    struct InputFacts {
        trace_graph::source::ObservedRegion region;
        trace_graph::planning::Phase phase;
        bool plain;
        bool deterministic_sampling;
    };
    std::multimap<std::vector<trace_graph::source::Index>, InputFacts> schedule_inputs_;
    struct PendingInput {
        trace_graph::source::Index frame, preparation;
        nlohmann::json metadata;
    };
    std::vector<PendingInput> pending_inputs_;
    std::map<std::string, std::string> sequence_dtypes_;
    std::map<std::string, trace_graph::source::Time> logits_output_ends_;
    std::map<trace_graph::source::Index, trace_graph::source::Time> sampling_ends_;
    struct EmbeddingWindow {
        trace_graph::source::Time lookup_end, reduction_begin;
    };
    std::map<std::string, EmbeddingWindow> embedding_windows_;
    std::map<std::pair<std::string, std::vector<trace_graph::source::Index>>, trace_graph::source::Index> collective_groups_;
    std::map<std::string, trace_graph::planning::PrefixId> token_descriptors_;
    std::map<std::tuple<std::string, trace_graph::planning::Phase, trace_graph::source::Index>, AttentionDescriptor> attention_descriptors_;
};

} // namespace markov::adapters::sglang
