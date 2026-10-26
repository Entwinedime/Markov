#include "markov/graph/source_graph.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

#include "markov/contracts/input_group.hpp"

namespace markov {

struct SourceGraph::ShapeReport {
    std::size_t checks = 0;
    nlohmann::json gaps = nlohmann::json::array();
};

SourceGraph::SourceGraph(nlohmann::json source) : source_(std::move(source)) {
    for (const auto & record : source_.at("values")) {
        if (record.at("id").get<std::size_t>() != values_.size()) throw std::invalid_argument("non-contiguous value identity");

        ValueAccess access{ record.at("storage").get<std::size_t>(), std::nullopt };
        if (!record.at("producer").is_null()) access.producer = record.at("producer").get<std::size_t>();
        value_accesses_.push_back(access);

        Geometry value{ record.at("shape").get<std::vector<std::int64_t>>(), std::nullopt };
        if (!record.at("shape_relation").is_null()) {
            value.shape_relation.emplace();
            for (const auto & relation : record.at("shape_relation")) value.shape_relation->push_back(Expression::read(relation));
            if (value.shape_relation->size() != value.shape.size()) throw std::invalid_argument("shape relation rank mismatch");
        }
        values_.push_back(std::move(value));
    }

    for (const auto & record : source_.at("operations")) {
        if (record.at("id").get<std::size_t>() != operations_.size()) throw std::invalid_argument("non-contiguous operation identity");
        operations_.push_back({ record.at("name").get<std::string>(),
                                record.at("region").get<std::size_t>(),
                                record.at("inputs").get<std::vector<std::size_t>>(),
                                record.at("outputs").get<std::vector<std::size_t>>(),
                                record.at("writes").get<std::vector<std::size_t>>() });
    }

    // Validate both access ordering and the reverse link from each output to its producer.
    dependency_count_ = connect_accesses(value_accesses_, operations_).size();
    for (std::size_t identity = 0; identity < operations_.size(); ++identity) {
        for (const auto output : operations_[identity].outputs) {
            if (value_accesses_.at(output).producer != identity) throw std::invalid_argument("output value producer mismatch");
        }
    }
}

Parameters SourceGraph::parameters(std::size_t region) const {
    Parameters bindings;

    // Walk outward so the nearest scope keeps precedence over its parents and batch.
    while (true) {
        const auto & record = source_.at("regions").at(region);
        const auto & metadata = record.at("metadata");
        for (const auto * key : { "parameters", "bindings" }) {
            if (metadata.contains(key)) {
                const auto local = metadata.at(key).get<Parameters>();
                bindings.insert(local.begin(), local.end());
            }
        }

        if (record.at("metadata").contains("batch")) {
            const auto batch = record.at("metadata").at("batch").get<std::size_t>();
            auto batch_parameters = source_.at("batches").at(batch).at("parameters").get<Parameters>();
            bindings.merge(batch_parameters);
            return bindings;
        }

        if (record.at("parent").is_null()) return bindings;
        const auto parent = record.at("parent").get<std::size_t>();
        if (parent >= region) throw std::invalid_argument("region parent must precede its child");
        region = parent;
    }
}

void SourceGraph::inspect_guards(const nlohmann::json & metadata, const Parameters & bindings, const nlohmann::json & location, ShapeReport & report) const {
    for (const auto * kind : { "guards", "execution_guards" }) {
        if (!metadata.contains(kind)) continue;
        for (const auto & guard : metadata.at(kind)) {
            if ((Expression::read(guard.at("condition")).evaluate(bindings) != 0) == guard.at("expected").get<bool>()) continue;

            auto gap = location;
            gap["reason"] = std::string(kind) + " condition disagrees with native input";
            report.gaps.push_back(std::move(gap));
        }
    }
}

void SourceGraph::inspect_shape(std::size_t identity, const Parameters & bindings, const nlohmann::json & location, ShapeReport & report) const {
    const auto & value = values_.at(identity);
    auto gap = location;
    gap["value"] = identity;

    if (!value.shape_relation) {
        gap["reason"] = "shape relation unavailable";
        report.gaps.push_back(std::move(gap));
        return;
    }

    for (std::size_t axis = 0; axis < value.shape.size(); ++axis) {
        ++report.checks;
        const auto predicted = value.shape_relation->at(axis).evaluate(bindings);
        if (predicted != value.shape[axis]) {
            auto mismatch = gap;
            mismatch.update({
                {     "axis",              axis },
                { "observed", value.shape[axis] },
                { "relation",         predicted }
            });
            report.gaps.push_back(std::move(mismatch));
        }
    }
}

nlohmann::json SourceGraph::summary() const {
    std::map<std::string, std::size_t> names;
    std::map<std::string, std::size_t> parameterized_outputs;
    std::map<std::string, std::size_t> native_interfaces;
    std::size_t interface_internal_operations = 0;
    ShapeReport shapes;
    auto & gaps = shapes.gaps;

    for (const auto & region : source_.at("regions")) {
        const auto & metadata = region.at("metadata");

        // Validate declared boundaries before interpreting shapes inside each region.
        if (metadata.contains("entrypoint_phase")) {
            const auto & phase = metadata.at("entrypoint_phase");
            if (!metadata.contains("entrypoint") || (!phase.is_null() && (!phase.is_string() || phase.get<std::string>().empty())))
                throw std::invalid_argument("entrypoint phase must be null or a nonempty string on an entrypoint");
        }

        if (metadata.contains("ports")) {
            if (metadata.at("ports").size() != 2) throw std::invalid_argument("ports require inputs and outputs");
            for (const auto * direction : { "inputs", "outputs" }) {
                for (const auto & [_, value] : metadata.at("ports").at(direction).items()) value_accesses_.at(value.get<std::size_t>());
            }
        }

        if (metadata.contains("input_groups")) {
            auto bindings = parameters(region.at("id").get<std::size_t>());
            const auto writes = metadata.value("writes", std::set<std::size_t>{});
            for (const auto & [name, specification] : metadata.at("input_groups").items()) {
                const auto group = InputGroup::read(specification);
                std::vector<Parameters> members;
                for (const auto & member : specification.at("members")) members.push_back(member.at("parameters").get<Parameters>());
                group.derive(bindings, members);

                if (metadata.contains("group_repetitions") && metadata.at("group_repetitions").contains(name)) {
                    if (region.at("parent").is_null()) throw std::invalid_argument("group formation needs a caller");
                    if (metadata.contains("parent_groups") && metadata.at("parent_groups").contains(name))
                        throw std::invalid_argument("input group cannot have two parent bindings: " + name);

                    const auto & repetition = metadata.at("group_repetitions").at(name);
                    const auto count = Expression::read(repetition.at("count")).evaluate(parameters(region.at("parent").get<std::size_t>()));
                    if (count < 0 || static_cast<std::size_t>(count) != members.size())
                        throw std::invalid_argument("group repetition disagrees with its caller count: " + name);
                }

                for (std::size_t index = 0; index < members.size(); ++index) {
                    const auto value = specification.at("members")[index].at("value").get<std::size_t>();
                    const auto & record = source_.at("values").at(value);
                    if (record.at("dtype") != group.member_dtype(index) || record.at("device") != group.device
                        || values_.at(value).shape != group.dimensions(bindings, members[index]))
                        gaps.push_back({
                            {      "region",                              region.at("id") },
                            { "input_group",                                         name },
                            {      "reason", "input group geometry disagrees with source" }
                        });
                    if (writes.contains(value_accesses_.at(value).storage)) throw std::invalid_argument("input group must be read-only");
                }
            }
        }

        // Relations must reproduce the captured invocation before they can describe a target.
        if (metadata.contains("requirements")) {
            for (const auto & requirement : metadata.at("requirements"))
                gaps.push_back({
                    { "region", region.at("id") },
                    { "reason",     requirement }
                });
        }

        if (metadata.contains("guards") || metadata.contains("execution_guards")) {
            inspect_guards(metadata,
                           parameters(region.at("id").get<std::size_t>()),
                           {
                               { "region", region.at("id") }
            },
                           shapes);
        }

        if (metadata.contains("derived_parameters")) {
            const auto observed = parameters(region.at("id").get<std::size_t>());
            auto computed = observed;
            const auto relations = ParameterRelations::read(metadata.at("derived_parameters"));
            relations.derive(computed);

            for (const auto & [name, _] : relations.expressions()) {
                if (computed.at(name) != observed.at(name))
                    gaps.push_back({
                        {    "region",                                 region.at("id") },
                        { "parameter",                                            name },
                        {  "observed",                               observed.at(name) },
                        {  "computed",                               computed.at(name) },
                        {    "reason", "derived parameter disagrees with native state" }
                    });
            }
        }

        std::set<std::size_t> declared_shapes;
        if (metadata.contains("shape_contracts")) {
            const auto bindings = parameters(region.at("id").get<std::size_t>());
            for (const auto & contract : metadata.at("shape_contracts")) {
                const auto value = contract.at("value").get<std::size_t>();
                declared_shapes.insert(value);
                const auto & observed = values_.at(value).shape;
                const auto & declared = contract.at("shape");
                if (declared.size() != observed.size()) throw std::invalid_argument("shape contract rank mismatch");
                for (std::size_t axis = 0; axis < observed.size(); ++axis) {
                    ++shapes.checks;
                    if (Expression::read(declared[axis]).evaluate(bindings) != observed[axis])
                        gaps.push_back({
                            { "region",                                    region.at("id") },
                            { "reason", "declared input shape disagrees with native value" }
                        });
                }
            }
        }

        if (!region.at("completed").get<bool>())
            gaps.push_back({
                { "region",                  region.at("id") },
                { "reason", "native region did not complete" }
            });

        // Native interfaces expose only declared outputs; private operations are checked below.
        if (metadata.contains("interface")) {
            ++native_interfaces[metadata.at("interface").get<std::string>()];
            const auto bindings = parameters(region.at("id").get<std::size_t>());
            const nlohmann::json location{
                { "region", region.at("id") }
            };
            std::set<std::size_t> opaque;
            if (metadata.contains("opaque_outputs")) opaque = metadata.at("opaque_outputs").get<std::set<std::size_t>>();

            // A declared port uses this call's parameters; its producer may belong to another batch.
            for (const auto & output : metadata.at("outputs")) {
                const auto value = output.get<std::size_t>();
                if (!opaque.contains(value) && !declared_shapes.contains(value)) inspect_shape(value, bindings, location, shapes);
            }

            if (metadata.contains("mutated_values")) {
                for (const auto & output : metadata.at("mutated_values")) {
                    const auto value = output.get<std::size_t>();
                    if (!opaque.contains(value) && !declared_shapes.contains(value)) inspect_shape(value, bindings, location, shapes);
                }
            }
        }

        const auto error = metadata.find("shape_error");
        if (error != metadata.end() && !error->is_null())
            gaps.push_back({
                { "region", region.at("id") },
                { "reason",          *error }
            });
    }

    for (std::size_t identity = 0; identity < operations_.size(); ++identity) {
        const auto & operation = operations_[identity];
        ++names[operation.name];
        const auto & error = source_.at("operations").at(identity).at("shape_error");
        if (!operation.outputs.empty() && !error.is_null())
            gaps.push_back({
                { "operation", identity },
                {    "reason",    error }
            });

        if (source_.at("operations").at(identity).value("shape_analysis", "") == "interface_internal") {
            const auto owner = source_.at("operations").at(identity).at("interface_region").get<std::size_t>();
            const auto & region = source_.at("regions").at(owner);
            if (!region.at("metadata").contains("interface")) throw std::invalid_argument("private operation has no owning native interface");
            ++interface_internal_operations;
            continue;
        }

        const auto bindings = parameters(operation.region);
        inspect_guards(source_.at("operations").at(identity),
                       bindings,
                       {
                           { "operation", identity }
        },
                       shapes);
        for (const auto output : operation.outputs) {
            const auto & value = values_.at(output);
            if (value.shape_relation && std::ranges::any_of(*value.shape_relation, [](const auto & expression) { return expression.parameterized(); }))
                ++parameterized_outputs[operation.name];
            inspect_shape(output,
                          bindings,
                          {
                              { "operation", identity }
            },
                          shapes);
        }
    }

    return {
        {                       "batches",  source_.at("batches").size() },
        {                       "regions",  source_.at("regions").size() },
        {                    "operations",            operations_.size() },
        {                        "values",                values_.size() },
        {                  "dependencies",             dependency_count_ },
        {               "operation_names",                         names },
        {         "parameterized_outputs",         parameterized_outputs },
        {             "native_interfaces",             native_interfaces },
        { "interface_internal_operations", interface_internal_operations },
        {                         "scope",     "observed_operator_graph" },
        {                  "shape_checks",                 shapes.checks },
        {                 "contract_gaps",                          gaps },
        {                "alias_ordering",  "whole_storage_conservative" }
    };
}

const nlohmann::json & SourceGraph::observations() const { return source_; }
const std::vector<ValueAccess> & SourceGraph::value_accesses() const { return value_accesses_; }

} // namespace markov
