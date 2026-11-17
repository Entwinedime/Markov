#pragma once

#include "markov/adapters/ktransformers/observations.hpp"
#include "markov/trace_graph/planning/program.hpp"

#include <deque>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace markov::adapters::ktransformers {

struct PredictionTarget {
    std::string name;
    nlohmann::json configuration;
    std::size_t cpu_threads, numa_pools, resident_experts;
    std::optional<std::string> infeasible;
};

struct PreparedPrediction {
    PreparedSource source;
    std::vector<Batch> batches;
    std::vector<std::size_t> request_tails;
    nlohmann::json deployment;
    std::map<std::size_t, SelectedCosts> costs;
    std::map<std::size_t, std::filesystem::path> models_paths;
    std::vector<PredictionTarget> targets;
    std::vector<trace_graph::cost::ScalingRule> node_scale;
    bool record_graph;
    nlohmann::json resource_envelope;
};

struct RequestResult {
    std::optional<trace_graph::graph::NodeId> start, response;
    bool completed = false;
};

// A complete forward is closed before result processing. Internal CPU/NPU
// work has no external interleaving in this serial, non-deferred eager scope.
class Program final : public trace_graph::planning::Program {
public:
    Program(const PreparedPrediction & input, const PredictionTarget & target) : input_(input), target_(target) {}
    void start(trace_graph::planning::ProgramRuntime & runtime) override;
    void on_event(trace_graph::planning::ProgramRuntime &, const trace_graph::simulation::Event & event) override;
    bool complete() const override;
    const std::vector<RequestResult> & requests() const { return requests_; }

private:
    const PreparedPrediction & input_;
    const PredictionTarget & target_;
    std::deque<trace_graph::planning::Work> work_;
    std::vector<RequestResult> requests_;
};

PreparedPrediction prepare_prediction(const nlohmann::json & raw, trace_graph::source::ProfileManifest manifest);

} // namespace markov::adapters::ktransformers
