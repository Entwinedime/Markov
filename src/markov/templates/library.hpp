#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "markov/graph/graph.hpp"
#include "markov/graph/types.hpp"

namespace markov {

// A caller sees a direct child's exported boundary, never its private event numbering.
struct TemplateReference {
    std::size_t event;
    std::optional<std::size_t> child;
    bool imported;

    static TemplateReference read(const nlohmann::json & source);
    nlohmann::json document() const;
};

struct TemplateResource {
    ResourceKind kind;
    std::string role;
    bool serial;

    bool operator==(const TemplateResource &) const = default;
};

struct TemplateWork {
    std::size_t resource;
    WorkKind kind;
    TemplateReference start;
    TemplateReference finish;
};

struct TemplateInterval {
    std::size_t definition;
    std::size_t interval;
};

struct TemplateEdge {
    TemplateReference source;
    TemplateReference target;
    Relation kind;
};

struct TensorRule {
    std::string dtype;
    std::vector<Expression> shape;

    TensorLayout evaluate(const Parameters & parameters) const;
};

struct TemplatePort {
    std::string name;
    Direction direction;
    PortUse use;
    Relation relation;
    TemplateReference event;
    std::optional<TensorRule> tensor;
    std::optional<StreamWait<TemplateReference>> wait{};
    std::optional<TemplateReference> rendezvous{};

    nlohmann::json document() const;
};

struct TemplateInterface {
    std::string role;
    std::string implementation;
    std::set<std::string> parameters;
    std::set<std::string> fixed;
    std::vector<TemplateResource> resources;

    // Imports, exports and named ports address boundary slots; each boundary binds one resource slot.
    std::vector<std::size_t> imports;
    std::vector<std::size_t> exports;
    std::vector<std::size_t> boundaries;
    nlohmann::json ports;

    // Caller-visible reachability is invariant across implementations sharing this interface.
    std::vector<std::pair<std::size_t, std::size_t>> causality;

    static TemplateInterface read(const nlohmann::json & source);
    nlohmann::json document() const;
    bool operator==(const TemplateInterface &) const = default;
};

struct TemplateCall {
    TemplateInterface contract;
    std::map<std::string, Expression> arguments;
    std::vector<TemplateReference> imports;
};

struct TemplateKvAccess {
    StateAccess access;
    TokenRangeRule rule;
    TemplateReference start;
    TemplateReference finish;
};

struct TemplateSlotAccess {
    std::string field;
    StateAccess access;
    std::string members;
    TemplateReference start;
    TemplateReference finish;
    std::string publication;
};

struct TemplateDefinition {
    std::string role;
    std::string implementation;
    std::set<std::string> parameters;
    std::set<std::string> fixed;
    std::vector<TemplateResource> resources;
    std::vector<std::size_t> events;
    std::vector<std::size_t> imports;
    std::vector<TemplateReference> exports;
    std::vector<TemplateWork> work;
    std::vector<TemplateEdge> edges;
    std::vector<TemplatePort> ports;
    std::vector<TemplateCall> children;
    std::vector<TemplateKvAccess> kv_accesses;
    std::vector<TemplateSlotAccess> slot_accesses;

    static TemplateDefinition read(const nlohmann::json & source);
    nlohmann::json document() const;
};

// Concrete sites, model constants and aliases are bindings, never part of shared identity.
struct TensorBinding {
    std::string value;
    std::string storage;
    std::int64_t version;
    bool external;

    // A pre-existing allocation may still receive its current value inside this action.
    std::optional<std::vector<std::size_t>> writer;
    std::string buffer;
};

struct KvBinding {
    std::string pool;
    std::int64_t layer;
    KvSlice slice;

    bool operator==(const KvBinding &) const = default;
};

// Access rules are shared; pool identities and byte placement belong to each invocation.
struct StateBindings {
    std::vector<KvBinding> kv;
    std::vector<std::string> slots;

    static StateBindings read(const nlohmann::json & source);
    nlohmann::json document() const;
    nlohmann::json contract() const;
    bool operator==(const StateBindings &) const = default;
};

struct TemplateFrame {

    // Implementation selection belongs to this call; the shared caller only specifies its interface.
    std::size_t definition = 0;
    Expression guard;
    std::string site;
    nlohmann::json bindings;
    Parameters constants;
    std::vector<std::size_t> resources;
    std::vector<std::optional<TensorBinding>> tensors;
    std::vector<TemplateFrame> children;
    StateBindings state;
    std::optional<std::string> state_input;
    std::optional<CommunicationGroup> communication;

    static TemplateFrame read(const nlohmann::json & source);
    nlohmann::json local_document() const;
    nlohmann::json document() const;
    std::map<std::string, StateBindings> state_inputs() const;
};

struct GraphInvocation {
    std::string action;
    nlohmann::json condition;
    Parameters parameters;
    std::vector<Resource> resources;
    nlohmann::json work;
    std::string identity;
    nlohmann::json environment;
    nlohmann::json domain;
    std::map<std::string, StorageBinding> storage;
    std::map<std::string, std::string> signals{};
    std::map<std::string, std::string> pools{};
    std::map<std::string, StateBindings> state_inputs{};
};

struct TemplateEntry {
    std::string action;
    nlohmann::json condition;
    TemplateFrame frame;

    // Distinct source parameter points qualify selection; source graphs retain individual call provenance.
    std::set<Parameters> observations;
};

// A local implementation carries no caller tensor identities, communication ranks or physical resources.
struct LocalImplementation {
    nlohmann::json contract;
    std::size_t definition;
    Expression guard;
};

class TemplateLibrary {
public:
    static TemplateLibrary read(const nlohmann::json & source);
    nlohmann::json document() const;
    void validate() const;
    TemplateInterface call_interface(const TemplateFrame & frame) const;
    nlohmann::json local_contract(const TemplateFrame & frame, const std::vector<std::size_t> & position) const;
    void collect_local_implementations();
    static void link_local_implementations(std::span<TemplateLibrary *> libraries);

    // Resource slots are explicitly bound by the adapter's operation declarations, then validated and normalized here.
    std::optional<std::size_t> import_leaf(const TemplateDefinition & definition, const TemplateEntry & source,
                                         std::span<const std::size_t> resource_bindings);
    CoarseGraph instantiate(const TemplateEntry & entry, const GraphInvocation & invocation,
                            std::vector<TemplateInterval> * origins = nullptr) const;

    nlohmann::json domain;
    nlohmann::json local_domain;
    std::vector<Resource> resources;
    std::vector<TemplateDefinition> definitions;
    std::vector<TemplateEntry> entries;
    std::vector<LocalImplementation> implementations;
};

} // namespace markov
