#include "markov/production/source_projection.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>

namespace markov {

struct SourceProjection::RuntimeFacts {
    nlohmann::json coverage;
    std::vector<nlohmann::json> effects;
};

struct SourceProjection::PhysicalFacts {
    nlohmann::json coverage;
    std::vector<nlohmann::json> work;
};

SourceProjection::SourceProjection(const SourceGraph & graph) : graph_(graph), source_(graph.observations()) {
    select();
    bind();
}

void SourceProjection::select() {
    const auto & regions = source_.at("regions");
    interfaces_.resize(regions.size());

    // An outer native interface owns its nested implementation as one public operation.
    for (std::size_t identity = 0; identity < regions.size(); ++identity) {
        const auto & region = regions[identity];
        if (region.at("id").get<std::size_t>() != identity) throw std::invalid_argument("non-contiguous region identity");
        if (!region.at("parent").is_null()) {
            const auto parent = region.at("parent").get<std::size_t>();
            if (parent >= identity) throw std::invalid_argument("region parent must precede its child");
            interfaces_[identity] = interfaces_[parent];
        }

        const auto & metadata = region.at("metadata");
        if (interfaces_[identity] && region.at("role") == "HostRead")
            throw std::invalid_argument("native interface hides a host read; keep its coordination boundary public");
        if (interfaces_[identity] || !metadata.contains("interface")) continue;

        interfaces_[identity] = identity;
        auto inputs = metadata.at("inputs").get<std::vector<std::size_t>>();
        auto outputs = metadata.at("outputs").get<std::vector<std::size_t>>();
        if (metadata.contains("ports")) {
            for (const auto & [_, value] : metadata.at("ports").at("inputs").items()) {
                const auto port_value = value.get<std::size_t>();
                if (std::ranges::find(inputs, port_value) == inputs.end()) inputs.push_back(port_value);
            }
            for (const auto & [_, value] : metadata.at("ports").at("outputs").items()) outputs.push_back(value.get<std::size_t>());
        }

        if (metadata.contains("mutated_values")) {
            for (const auto & value : metadata.at("mutated_values")) outputs.push_back(value.get<std::size_t>());
        }

        // Preserve interface order; source allocation identities cannot define a reusable output signature.
        std::set<std::size_t> seen_outputs;
        std::erase_if(outputs, [&](auto value) { return !seen_outputs.insert(value).second; });
        operations_.push_back({
            { metadata.at("interface").get<std::string>(),
             identity, std::move(inputs),
             std::move(outputs),
             metadata.at("writes").get<std::vector<std::size_t>>() },
            std::nullopt,
            region.at("operation_begin").get<std::size_t>(),
            nlohmann::json::object()
        });

        auto & properties = operations_.back().properties;
        for (const auto & key : { "arguments",
                                  "keywords",
                                  "attributes",
                                  "communication",
                                  "execution_guards",
                                  "execution_reduction",
                                  "opaque_inputs",
                                  "opaque_outputs",
                                  "input_groups" }) {
            if (metadata.contains(key)) properties[key] = metadata.at(key);
        }
        if (metadata.contains("argument_relations")) properties["arguments"] = metadata.at("argument_relations");
    }

    const auto & records = source_.at("operations");
    for (std::size_t identity = 0; identity < records.size(); ++identity) {
        const auto & record = records[identity];
        const auto region = record.at("region").get<std::size_t>();
        if (interfaces_.at(region)) continue;

        const auto & arguments = record.contains("argument_relations") ? record.at("argument_relations") : record.at("arguments");
        auto outputs = record.at("outputs").get<std::vector<std::size_t>>();
        if (record.contains("public_outputs")) {
            for (const auto & value : record.at("public_outputs")) {
                const auto identity_value = value.get<std::size_t>();
                const auto & alias = source_.at("values").at(identity_value);
                const auto same_write = std::ranges::any_of(record.at("outputs"), [&](const auto & output) {
                    const auto & actual = source_.at("values").at(output.template get<std::size_t>());
                    return actual.at("storage") == alias.at("storage") && actual.at("version") == alias.at("version");
                });
                if (!same_write) throw std::invalid_argument("public output is not an alias of this native write");
                if (std::ranges::find(outputs, identity_value) == outputs.end()) outputs.push_back(identity_value);
            }
        }
        operations_.push_back({
            { record.value("interface", record.at("name").get<std::string>()),
             region, record.at("inputs").get<std::vector<std::size_t>>(),
             std::move(outputs),
             record.at("writes").get<std::vector<std::size_t>>() },
            identity,
            identity,
            { { "arguments", arguments }, { "keywords", record.at("keywords") } }
        });
        if (record.contains("execution_guards")) operations_.back().properties["execution_guards"] = record.at("execution_guards");
    }

    std::ranges::sort(operations_, [](const auto & left, const auto & right) {
        return std::tuple{ left.position, left.source.has_value(), left.access.region }
               < std::tuple{ right.position, right.source.has_value(), right.access.region };
    });

    owners_.resize(records.size());
    for (std::size_t identity = 0; identity < operations_.size(); ++identity) {
        const auto & operation = operations_[identity];
        if (operation.source) owners_[*operation.source] = identity;
        else native_.emplace(operation.access.region, identity);
    }

    for (std::size_t identity = 0; identity < records.size(); ++identity) {
        if (const auto region = interfaces_.at(records[identity].at("region").get<std::size_t>())) owners_[identity] = native_.at(*region);
    }
}

void SourceProjection::bind() {
    const auto & records = source_.at("operations");
    values_ = graph_.value_accesses();
    for (auto & value : values_) {
        if (value.producer) value.producer = owners_.at(*value.producer);
    }

    for (std::size_t identity = 0; identity < operations_.size(); ++identity) {
        const auto & operation = operations_[identity];
        for (const auto output : operation.access.outputs) {

            // An unchanged returned input is an alias, not a new value producer.
            if (std::ranges::find(operation.access.inputs, output) == operation.access.inputs.end()) values_.at(output).producer = identity;
        }
    }

    // Classify births after public interface outputs acquire their actual producer.
    std::map<std::size_t, std::optional<std::size_t>> storage_owners;
    for (const auto & value : values_) storage_owners.try_emplace(value.storage, value.producer);

    for (const auto & region : source_.at("regions")) {
        const auto & metadata = region.at("metadata");
        if (!metadata.contains("private_storages")) continue;

        const auto & boundary = interfaces_.at(region.at("id").get<std::size_t>());
        if (!boundary) throw std::invalid_argument("private storage has no native interface owner");
        const auto identity = native_.at(*boundary);
        for (const auto & storage : metadata.at("private_storages")) storage_owners.insert_or_assign(storage.get<std::size_t>(), identity);
    }

    for (const auto & [storage, owner] : storage_owners) {
        if (!owner) external_resources_.push_back(storage);
    }

    // Preserve undeclared external storage dependencies without exposing private intermediates.
    std::vector<std::set<std::size_t>> seen_storage(operations_.size());
    for (std::size_t identity = 0; identity < operations_.size(); ++identity) {
        for (const auto input : operations_[identity].access.inputs) seen_storage[identity].insert(values_.at(input).storage);
    }

    for (std::size_t raw = 0; raw < records.size(); ++raw) {
        const auto owner = owners_[raw];
        auto & operation = operations_[owner];
        if (operation.source) continue;

        for (const auto & input_record : records[raw].at("inputs")) {
            const auto input = input_record.get<std::size_t>();
            if (values_.at(input).producer == owner) continue;
            const auto storage = values_.at(input).storage;
            if (storage_owners.at(storage) == owner || !seen_storage[owner].insert(storage).second) continue;

            operation.access.inputs.push_back(input);
            if (!operation.properties.contains("opaque_inputs")) operation.properties["opaque_inputs"] = nlohmann::json::array();
            operation.properties["opaque_inputs"].push_back(input);
        }
    }

    // A private intermediate may escape only through the native interface's declared output ports.
    for (std::size_t consumer = 0; consumer < operations_.size(); ++consumer) {
        for (const auto input : operations_[consumer].access.inputs) {
            const auto producer = values_.at(input).producer;
            if (!producer || *producer == consumer || operations_[*producer].source) continue;
            const auto & outputs = operations_[*producer].access.outputs;
            if (std::ranges::find(outputs, input) == outputs.end())
                throw std::invalid_argument("private value " + std::to_string(input) + " escapes interface region "
                                            + std::to_string(operations_[*producer].access.region) + " into " + operations_[consumer].access.name
                                            + " in region " + std::to_string(operations_[consumer].access.region));
        }
    }
}

std::optional<std::size_t> SourceProjection::owner(const nlohmann::json & record) const {
    if (!record.at("operation").is_null()) return owners_.at(record.at("operation").get<std::size_t>());
    if (record.at("region").is_null()) return std::nullopt;

    const auto region = record.at("region").get<std::size_t>();
    if (!interfaces_.at(region)) return std::nullopt;
    return native_.at(*interfaces_[region]);
}

SourceProjection::PhysicalFacts SourceProjection::physical_facts(const nlohmann::json * physical) const {
    PhysicalFacts result{ { { "status", "not_observed" } }, std::vector<nlohmann::json>(operations_.size(), nlohmann::json::array()) };
    if (physical == nullptr) return result;

    struct Unowned {
        std::size_t region;
        std::size_t source_event;
        std::size_t count = 0;
    };

    std::map<std::string, Unowned> unowned;
    std::set<std::size_t> attributed_operations;
    std::size_t observed = 0;
    std::size_t attributed = 0;
    std::size_t submitted = 0;
    std::size_t queued = 0;

    for (const auto & work : physical->at("work")) {
        if (work.at("observer").get<bool>() || work.at("auxiliary").get<bool>() || !work.at("baseline").is_null()) continue;
        if (work.at("operation").is_null() && work.at("region").is_null()) continue;

        ++observed;
        const auto & submission = work.at("submission");
        if (!submission.is_null()) {
            ++submitted;
            if (!submission.at("queue").is_null()) ++queued;
        }

        if (const auto identity = owner(work)) {
            attributed_operations.insert(*identity);
            nlohmann::json record;

            // Keep native identities in source evidence; measured time is not a target cost.
            for (const auto & key : { "name", "kind", "stream", "task_type", "source_event", "order", "submission" }) record[key] = work.at(key);
            if (!work.at("operation").is_null() && source_.at("operations").at(work.at("operation").get<std::size_t>()).value("communication", false))
                record["communication"] = true;
            result.work[*identity].push_back(std::move(record));
            ++attributed;
            continue;
        }

        const auto region = work.at("region").get<std::size_t>();
        auto [position, _] = unowned.try_emplace(work.at("name").get<std::string>(), region, work.at("source_event").get<std::size_t>());
        ++position->second.count;
    }

    nlohmann::json missing = nlohmann::json::array();
    for (const auto & [name, work] : unowned) {
        missing.push_back({
            {         "name",              name },
            {        "count",        work.count },
            {       "region",       work.region },
            { "source_event", work.source_event }
        });
    }

    const auto unassociated = physical->at("unassociated_tasks").size();
    result.coverage = {
        {                 "scope", "device task ownership; not resource-order or read/write qualification" },
        {                "status",   unowned.empty() && unassociated == 0 ? "fully_attributed" : "partial" },
        {        "observed_tasks",                                                                observed },
        {      "attributed_tasks",                                                              attributed },
        { "attributed_operations",                                            attributed_operations.size() },
        {    "unassociated_tasks",                                                            unassociated },
        {               "unowned",                                                      std::move(missing) }
    };
    result.coverage["submissions"] = {
        {           "status",
         observed == 0           ? "not_observed"
         : submitted == observed ? "fully_associated"
         : "partial"                            },
        { "associated_tasks",          submitted },
        {     "queued_tasks",             queued },
        {     "direct_tasks", submitted - queued }
    };

    return result;
}

SourceProjection::RuntimeFacts SourceProjection::runtime_facts(const nlohmann::json * physical) const {
    RuntimeFacts result{
        { { "status", "not_observed" }, { "native_arguments", { { "status", "not_observed" } } } },
        std::vector<nlohmann::json>(operations_.size(), nlohmann::json::array())
    };
    if (physical == nullptr) return result;

    std::size_t observed = 0;
    std::size_t attributed = 0;
    std::size_t unclassified = 0;
    std::size_t with_arguments = 0;
    std::map<std::string, std::size_t> missing_arguments;
    std::map<std::string, nlohmann::json> missing;

    for (const auto & action : physical->at("runtime_actions")) {
        if (action.at("observer").get<bool>() || action.at("auxiliary").get<bool>() || !action.at("baseline").is_null()) continue;
        if (action.at("operation").is_null() && action.at("region").is_null()) continue;

        ++observed;
        if (!action.at("native_call").is_null()) ++with_arguments;
        else ++missing_arguments[action.at("native_api").get<std::string>()];
        const auto kind = action.at("kind").get<std::string>();
        if (kind == "unclassified") ++unclassified;

        if (const auto identity = owner(action)) {
            ++attributed;
            result.effects[*identity].push_back(action);
            continue;
        }

        const auto [position, inserted] = missing.try_emplace(kind,
                                                              nlohmann::json{
                                                                  {         "kind",                      kind },
                                                                  {        "count",                         0 },
                                                                  {       "region",       action.at("region") },
                                                                  { "source_event", action.at("source_event") }
        });
        position->second["count"] = position->second.at("count").get<std::size_t>() + 1;
    }

    const auto unassociated = physical->at("unassociated_runtime_actions").size();
    auto unowned = nlohmann::json::array();
    for (auto & [_, action] : missing) unowned.push_back(std::move(action));
    result.coverage = {
        {                "scope", "runtime effect ownership; native arguments reported separately from execution qualification" },
        {               "status",    missing.empty() && unassociated == 0 && unclassified == 0 ? "fully_attributed" : "partial" },
        {     "observed_actions",                                                                                      observed },
        {   "attributed_actions",                                                                                    attributed },
        { "unclassified_actions",                                                                                  unclassified },
        { "unassociated_actions",                                                                                  unassociated },
        {              "unowned",                                                                            std::move(unowned) }
    };

    const auto argument_status = with_arguments == 0 ? "not_observed" : with_arguments == observed ? "complete" : "partial";
    result.coverage["native_arguments"] = {
        {           "status",              argument_status },
        { "observed_actions",               with_arguments },
        {  "unobserved_apis", std::move(missing_arguments) }
    };

    return result;
}

nlohmann::json SourceProjection::document(const nlohmann::json * physical) const {
    auto runtime = runtime_facts(physical);
    auto device = physical_facts(physical);

    std::map<std::pair<std::string, std::size_t>, const nlohmann::json *> host_scopes;
    if (physical != nullptr) {
        for (const auto & scope : physical->at("host_scopes")) {
            if (!host_scopes.emplace(std::pair{ scope.at("kind").get<std::string>(), scope.at("identity").get<std::size_t>() }, &scope).second)
                throw std::invalid_argument("public host scope has multiple native observations");
        }
    }

    nlohmann::json result{
        {  "environment", source_.at("environment") },
        {      "modules",     source_.at("modules") },
        {      "batches",     source_.at("batches") },
        {      "regions",   nlohmann::json::array() },
        {       "values",   nlohmann::json::array() },
        {   "operations",   nlohmann::json::array() },
        { "dependencies",   nlohmann::json::array() },
        {        "scope",     "public_source_graph" }
    };
    result["external_resources"] = external_resources_;
    result["runtime_coverage"] = std::move(runtime.coverage);
    result["physical_coverage"] = std::move(device.coverage);

    std::set<std::size_t> used;
    std::vector<Operation> accesses;
    for (std::size_t identity = 0; identity < operations_.size(); ++identity) {
        const auto & operation = operations_[identity];
        const auto & access = operation.access;
        auto record = operation.properties;
        const auto host =
            host_scopes.find(operation.source ? std::pair{ std::string("operation"), *operation.source } : std::pair{ std::string("region"), access.region });
        if (!device.work[identity].empty() || !runtime.effects[identity].empty() || host != host_scopes.end()) {
            record["execution_observation"] = {
                { "communication", record.value("communication", nlohmann::json(nullptr)) },
                { "host_scope", host == host_scopes.end() ? nlohmann::json(nullptr) : *host->second },
                { "device_work", std::move(device.work[identity]) },
                { "runtime_actions", std::move(runtime.effects[identity]) }
            };
        }

        record.update({
            {       "id",                                    identity },
            {     "name",                                 access.name },
            {   "region",                               access.region },
            { "position",                          operation.position },
            {   "inputs",                               access.inputs },
            {  "outputs",                              access.outputs },
            {   "writes",                               access.writes },
            {     "kind", operation.source ? "operator" : "interface" }
        });
        result["operations"].push_back(std::move(record));
        used.insert(access.inputs.begin(), access.inputs.end());
        used.insert(access.outputs.begin(), access.outputs.end());
        accesses.push_back(access);
    }

    // Keep values referenced by public boundaries and module state, even without an operation edge.
    for (const auto & region : source_.at("regions")) {
        const auto identity = region.at("id").get<std::size_t>();
        if (interfaces_[identity] && *interfaces_[identity] != identity) continue;

        for (const auto & key : { "inputs", "outputs" }) {
            if (!region.at("metadata").contains(key)) continue;
            for (const auto & value : region.at("metadata").at(key)) used.insert(value.get<std::size_t>());
        }

        if (region.at("metadata").contains("source")) used.insert(region.at("metadata").at("source").get<std::size_t>());
        if (region.at("metadata").contains("shape_contracts")) {
            for (const auto & contract : region.at("metadata").at("shape_contracts")) used.insert(contract.at("value").get<std::size_t>());
        }

        if (region.at("metadata").contains("ports")) {
            for (const auto & ports : region.at("metadata").at("ports")) {
                for (const auto & value : ports) used.insert(value.get<std::size_t>());
            }
        }
    }

    for (const auto & module : source_.at("modules")) {
        for (const auto & parameter : module.at("parameters")) used.insert(parameter.at("value").get<std::size_t>());
    }

    for (const auto identity : used) {
        auto value = source_.at("values").at(identity);
        const auto producer = values_[identity].producer;
        value["producer"] = producer ? nlohmann::json(*producer) : nlohmann::json(nullptr);
        result["values"].push_back(std::move(value));
    }

    for (const auto & region : source_.at("regions")) {
        nlohmann::json metadata = nlohmann::json::object();
        for (const auto & key : { "path",          "entrypoint",        "batch",          "no_batch",
                                  "inputs",        "outputs",           "mutated_values", "input_groups",
                                  "opaque_inputs", "opaque_outputs",    "parameters",     "derived_parameters",
                                  "state_effects", "bindings",          "effects",        "source",
                                  "requests",      "generated_tokens",  "guards",         "shape_contracts",
                                  "parent_groups", "group_repetitions", "ports",          "entrypoint_phase",
                                  "dynamic_calls", "call_role",         "call_count",     "call_family" }) {
            if (region.at("metadata").contains(key)) metadata[key] = region.at("metadata").at(key);
        }

        const auto & interface = interfaces_[region.at("id").get<std::size_t>()];
        const auto owner = interface ? nlohmann::json(*interface) : nlohmann::json(nullptr);
        const auto host = host_scopes.find({ "region", region.at("id").get<std::size_t>() });
        result["regions"].push_back({
            {              "id",                                                     region.at("id") },
            {          "parent",                                                 region.at("parent") },
            {            "role",                                                   region.at("role") },
            { "operation_begin",                                        region.at("operation_begin") },
            { "interface_owner",                                                               owner },
            {      "host_scope", host == host_scopes.end() ? nlohmann::json(nullptr) : *host->second },
            {        "metadata",                                                 std::move(metadata) }
        });
    }

    for (const auto & edge : connect_accesses(values_, accesses)) {
        result["dependencies"].push_back({
            { "source", edge.source },
            { "target", edge.target },
            {   "kind",   edge.kind }
        });
    }

    return result;
}

} // namespace markov
