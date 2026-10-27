#pragma once

#include "markov/trace_graph/planning/work.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace markov::trace_graph::source {

using Time = std::int64_t;
using Index = std::size_t;

enum class ResourceKind { CpuThread, DeviceStream, Client };

struct Resource {
    ResourceKind kind;
    std::string identity;
};

struct RecordRef {
    Index file;
    Index position;
};

enum class SpanRole { Service, Wait, ProfilerOverhead };

struct ObservedSpan {
    Time begin_ns;
    Time end_ns;
    Index resource;
    Index name;
    RecordRef record;
    SpanRole role = SpanRole::Service;
};

struct ObservedPoint {
    Time time_ns;
    Index resource;
    Index name;
    RecordRef record;
};

enum class DependencyKind { Flow, Submission, Synchronization };

struct ObservedFlow {
    std::string identity;
    Index point;
    bool start;
    DependencyKind kind = DependencyKind::Flow;
};

struct ObservedDependency {
    Index before;
    Index after;
    DependencyKind kind;
};

struct FlowDiagnostic {
    std::string identity;
    std::size_t starts;
    std::size_t ends;
};

struct RequestWindow {
    std::string identity;
    Time begin_ns;
    Time end_ns;
    bool formal;
    std::optional<Index> received = std::nullopt;
    std::optional<Index> sent = std::nullopt;
};

struct ObservedWork {
    Index description;
    Index resource;
    Time begin_ns;
    Time end_ns;
    RecordRef record;
};

// Source-only observations. No target policy, regression or mutable execution
// state lives here. Resource and name IDs remain stable for the capture lifetime.
class Observations {
public:
    Index name(const std::string & value);
    Index resource(ResourceKind kind, const std::string & identity);
    Index point(ObservedPoint value);
    void span(ObservedSpan value);
    void alias_resource(Index from, Index to);
    Index canonical_resource(Index resource) const;
    void normalize_resources();
    void resolve_flows();
    void release_intake_indexes();
    Index describe_work(planning::Work work);

    std::vector<std::string> names;
    std::vector<Resource> resources;
    std::vector<ObservedSpan> spans;
    std::vector<ObservedPoint> points;
    std::vector<ObservedFlow> flows;
    std::vector<ObservedDependency> dependencies;
    std::map<std::string, std::size_t> flow_audit;
    std::vector<FlowDiagnostic> unresolved_flows;
    std::vector<RequestWindow> requests;
    std::vector<planning::Work> work_descriptions;
    std::vector<ObservedWork> work_observations;
    std::map<std::string, std::size_t> counts;

private:
    std::unordered_map<std::string, Index> name_ids_;
    std::unordered_map<std::string, Index> resource_ids_;
    std::unordered_map<Index, Index> resource_aliases_;
    std::map<planning::Work, Index> work_ids_;
};

} // namespace markov::trace_graph::source
