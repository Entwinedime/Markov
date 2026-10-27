#pragma once

#include "markov/adapters/ascend/causality.hpp"
#include "markov/trace_graph/source/chrome_reader.hpp"
#include "markov/trace_graph/source/manifest.hpp"
#include "markov/trace_graph/source/observations.hpp"

#include <map>
#include <optional>
#include <set>
#include <string>

namespace markov::adapters::ascend {

// Translate native display identities and Chrome flow ports into neutral
// observations. Framework probe facts are handled by a separate adapter.
class TraceReader final : public trace_graph::source::RecordConsumer {
public:
    TraceReader(trace_graph::source::Observations & output, RuntimeCausality & causality, const trace_graph::source::ProfileFile & file, std::size_t file_id);
    void record(std::size_t position, const nlohmann::json & event) override;
    void finish();

private:
    trace_graph::source::Index lane(const std::string & process, const std::string & thread, bool device);

    trace_graph::source::Observations & output_;
    RuntimeCausality & causality_;
    const trace_graph::source::ProfileFile & file_;
    std::size_t file_id_;
    std::set<std::string> host_processes_;
    std::set<std::string> runtime_processes_;
    std::set<std::string> device_processes_;
    std::map<std::pair<std::string, std::string>, trace_graph::source::Index> cpu_lanes_;
    std::map<std::string, trace_graph::source::Index> device_lanes_;
    struct ExportedKernelFlow {
        std::string native_id;
        std::string identity;
        std::size_t position;
    };
    std::optional<ExportedKernelFlow> kernel_flow_;
};

} // namespace markov::adapters::ascend
