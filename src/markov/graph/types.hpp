#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "markov/contracts/expression.hpp"

namespace markov {

enum class ResourceKind { Thread, Stream, Service };
enum class WorkKind { Host, Device, Transfer, Communication, Wait, Boundary };
enum class Relation { Order, Submission, Data, Signal, Control, State, Communication };
enum class Direction { Input, Output };
enum class PortUse { Control, Data, Metadata, Signal };

// Snapshot accepted commands on the peer stream; host queueing and device submission can finish later.
template <class Boundary> struct StreamWait {
    std::size_t resource;
    Boundary begin;
};

ResourceKind resource_kind(std::string_view name);
std::string_view resource_name(ResourceKind kind);
WorkKind work_kind(std::string_view name);
std::string_view work_name(WorkKind kind);
Relation relation_kind(std::string_view name);
std::string_view relation_name(Relation kind);
Direction direction_kind(std::string_view name);
std::string_view direction_name(Direction direction);
PortUse port_use(std::string_view name);
std::string_view port_use_name(PortUse use);

// Participants declare which input-ready peers their local completion requires.
struct CommunicationGroup {
    std::string channel;
    std::string operation;
    std::vector<std::int64_t> members;
    std::vector<std::int64_t> required;
    std::int64_t rank;
    std::size_t order;

    static CommunicationGroup read(const nlohmann::json & source);
    nlohmann::json document() const;
    void validate() const;
};

struct Resource {
    ResourceKind kind;
    std::int64_t rank;
    std::string role;
    std::string identity;
    bool serial;

    bool operator==(const Resource &) const = default;

    static Resource read(const nlohmann::json & source);
    nlohmann::json document() const;
};

// Actual geometry is evidence; a rule is a separate declaration for template construction.
struct TensorLayout {
    std::string dtype;
    std::vector<std::int64_t> shape;

    bool operator==(const TensorLayout &) const = default;
};

struct StorageBinding {
    std::string identity;
    std::int64_t version;
};

struct TensorObservation {
    std::string value;
    std::string storage;
    std::int64_t version;
    std::string dtype;
    std::vector<std::int64_t> shape;
    std::optional<std::vector<Expression>> shape_rule;
    std::optional<std::size_t> writer{};

    // Mutation changes the writer, never the allocation's owner.
    std::optional<std::size_t> allocation{};

    // An adapter-declared persistent storage owner, independent of the observing callsite.
    std::string buffer{};

    static TensorObservation read(const nlohmann::json & source);
    nlohmann::json document() const;
};

struct ScopeInfo {
    std::optional<std::size_t> parent;
    std::string role;
    std::string site;
    std::string implementation;
    Parameters parameters;
    Parameters constants;
    std::map<std::string, Expression> arguments;
    nlohmann::json work;
    nlohmann::json bindings;
    std::vector<Expression> guards{};

    static ScopeInfo read(const nlohmann::json & source);
    nlohmann::json document() const;
};

} // namespace markov
