#pragma once

#include "markov/contracts/control.hpp"
#include "markov/graph/source_graph.hpp"

#include <cstddef>
#include <map>
#include <optional>
#include <span>
#include <string>

namespace markov {

struct SourceCall;

// Framework interpretation is injected into runtime-specific source graph extraction.
class SourceBindings {
public:
    virtual ~SourceBindings() = default;

    virtual std::map<PortKind, BoundaryId> ports(const SourceGraph & graph, std::span<const SourceCall> calls, std::size_t call_index) const = 0;
    // A fallback name for an observed region; it does not request a new cut.
    virtual std::optional<std::string> cpu_name(const SourceCall & call) const = 0;
    virtual bool cpu_work_boundary(const SourceCall & call) const = 0;
};

} // namespace markov
