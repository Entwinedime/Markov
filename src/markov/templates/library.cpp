#include "markov/templates/library.hpp"

#include <stdexcept>
#include <utility>

namespace markov {
namespace {

std::size_t index(const nlohmann::json & value) {
    if (!value.is_number_integer() || value.get<std::int64_t>() < 0) throw std::invalid_argument("template index must be a nonnegative integer");
    return value.get<std::size_t>();
}

std::vector<std::size_t> indices(const nlohmann::json & source) {
    std::vector<std::size_t> result;
    for (const auto & item : source.get_ref<const nlohmann::json::array_t &>()) result.push_back(index(item));
    return result;
}

} // namespace

TensorLayout TensorRule::evaluate(const Parameters & parameters) const {
    TensorLayout result{ dtype, {} };
    for (const auto & expression : shape) {
        const auto dimension = expression.evaluate(parameters);
        if (dimension < 0) throw std::invalid_argument("template produces a negative tensor dimension");
        result.shape.push_back(dimension);
    }
    return result;
}

TemplateReference TemplateReference::read(const nlohmann::json & source) {
    const auto & child = source.at("child");
    return { index(source.at("event")), child.is_null() ? std::nullopt : std::optional(index(child)), source.at("imported").get<bool>() };
}

nlohmann::json TemplateReference::document() const {
    return {
        {    "event",                                                    event },
        {    "child", child ? nlohmann::json(*child) : nlohmann::json(nullptr) },
        { "imported",                                                 imported }
    };
}

nlohmann::json TemplatePort::document() const {
    nlohmann::json tensor_json = nullptr;
    if (tensor) {
        tensor_json = {
            { "dtype",           tensor->dtype },
            { "shape", nlohmann::json::array() }
        };
        for (const auto & dimension : tensor->shape) tensor_json["shape"].push_back(dimension.document());
    }
    nlohmann::json result{
        {      "name",                      name },
        { "direction", direction_name(direction) },
        {       "use",        port_use_name(use) },
        {  "relation",   relation_name(relation) },
        {     "event",          event.document() },
        {    "tensor",    std::move(tensor_json) }
    };
    if (rendezvous) result["rendezvous"] = rendezvous->document();
    if (wait)
        result["wait"] = {
            { "resource",         wait->resource },
            {    "begin", wait->begin.document() }
        };
    return result;
}

TemplateInterface TemplateInterface::read(const nlohmann::json & source) {
    TemplateInterface result{ source.at("role").get<std::string>(),
                              source.at("implementation").get<std::string>(),
                              source.at("parameters").get<std::set<std::string>>(),
                              source.at("fixed").get<std::set<std::string>>(),
                              {},
                              indices(source.at("imports")),
                              indices(source.at("exports")),
                              indices(source.at("boundaries")),
                              source.at("ports") };
    for (const auto & resource : source.at("resources").get_ref<const nlohmann::json::array_t &>())
        result.resources.push_back(
            { resource_kind(resource.at("kind").get<std::string>()), resource.at("role").get<std::string>(), resource.at("serial").get<bool>() });
    return result;
}

nlohmann::json TemplateInterface::document() const {
    nlohmann::json result{
        {           "role",                    role },
        { "implementation",          implementation },
        {     "parameters",              parameters },
        {          "fixed",                   fixed },
        {        "imports",                 imports },
        {        "exports",                 exports },
        {     "boundaries",              boundaries },
        {          "ports",                   ports },
        {      "resources", nlohmann::json::array() }
    };
    for (const auto & resource : resources)
        result["resources"].push_back({
            {   "kind", resource_name(resource.kind) },
            {   "role",                resource.role },
            { "serial",              resource.serial }
        });
    return result;
}

TemplateDefinition TemplateDefinition::read(const nlohmann::json & source) {
    TemplateDefinition result;
    result.role = source.at("role").get<std::string>();
    result.implementation = source.at("implementation").get<std::string>();
    result.parameters = source.at("parameters").get<std::set<std::string>>();
    result.fixed = source.at("fixed").get<std::set<std::string>>();
    result.events = indices(source.at("events"));
    result.imports = indices(source.at("imports"));
    for (const auto & endpoint : source.at("exports").get_ref<const nlohmann::json::array_t &>()) result.exports.push_back(TemplateReference::read(endpoint));
    for (const auto & access : source.at("slot_accesses"))
        result.slot_accesses.push_back({ access.at("pool").get<std::string>(),
                                         access.at("field").get<std::string>(),
                                         state_access(access.at("access").get<std::string>()),
                                         access.at("members").get<std::string>(),
                                         TemplateReference::read(access.at("start")),
                                         TemplateReference::read(access.at("finish")) });
    for (const auto & access : source.at("kv_accesses").get_ref<const nlohmann::json::array_t &>())
        result.kv_accesses.push_back({ access.at("pool").get<std::string>(),
                                       state_access(access.at("access").get<std::string>()),
                                       KvSlice::read(access.at("slice")),
                                       TokenRangeRule::read(access.at("rule")),
                                       TemplateReference::read(access.at("start")),
                                       TemplateReference::read(access.at("finish")) });
    for (const auto & resource : source.at("resources").get_ref<const nlohmann::json::array_t &>())
        result.resources.push_back(
            { resource_kind(resource.at("kind").get<std::string>()), resource.at("role").get<std::string>(), resource.at("serial").get<bool>() });

    for (const auto & piece : source.at("work").get_ref<const nlohmann::json::array_t &>())
        result.work.push_back({ index(piece.at("resource")),
                                work_kind(piece.at("kind").get<std::string>()),
                                TemplateReference::read(piece.at("start")),
                                TemplateReference::read(piece.at("finish")) });
    for (const auto & edge : source.at("edges").get_ref<const nlohmann::json::array_t &>())
        result.edges.push_back(
            { TemplateReference::read(edge.at("source")), TemplateReference::read(edge.at("target")), relation_kind(edge.at("kind").get<std::string>()) });

    for (const auto & port : source.at("ports").get_ref<const nlohmann::json::array_t &>()) {
        TemplatePort boundary{ port.at("name").get<std::string>(),          direction_kind(port.at("direction").get<std::string>()),
                               port_use(port.at("use").get<std::string>()), relation_kind(port.at("relation").get<std::string>()),
                               TemplateReference::read(port.at("event")),   std::nullopt };
        if (port.contains("wait")) {
            const auto & wait = port.at("wait");
            boundary.wait = StreamWait<TemplateReference>{ index(wait.at("resource")), TemplateReference::read(wait.at("begin")) };
        }
        if (!port.at("tensor").is_null()) {
            const auto & tensor = port.at("tensor");
            boundary.tensor = TensorRule{ tensor.at("dtype").get<std::string>(), {} };
            for (const auto & dimension : tensor.at("shape").get_ref<const nlohmann::json::array_t &>())
                boundary.tensor->shape.push_back(Expression::read(dimension));
        }
        if (port.contains("rendezvous")) boundary.rendezvous = TemplateReference::read(port.at("rendezvous"));
        result.ports.push_back(std::move(boundary));
    }
    for (const auto & child : source.at("children").get_ref<const nlohmann::json::array_t &>()) {
        TemplateCall call{ TemplateInterface::read(child.at("contract")), {}, {} };
        const auto & arguments = child.at("arguments").get_ref<const nlohmann::json::object_t &>();
        for (const auto & [name, expression] : arguments) call.arguments.emplace(name, Expression::read(expression));
        for (const auto & imported : child.at("imports").get_ref<const nlohmann::json::array_t &>()) call.imports.push_back(TemplateReference::read(imported));
        result.children.push_back(std::move(call));
    }
    return result;
}

nlohmann::json TemplateDefinition::document() const {
    nlohmann::json result{
        {           "role",                    role },
        { "implementation",          implementation },
        {     "parameters",              parameters },
        {          "fixed",                   fixed },
        {         "events",                  events },
        {        "imports",                 imports },
        {        "exports", nlohmann::json::array() },
        {      "resources", nlohmann::json::array() },
        {           "work", nlohmann::json::array() },
        {          "edges", nlohmann::json::array() },
        {          "ports", nlohmann::json::array() },
        {       "children", nlohmann::json::array() }
    };
    result["slot_accesses"] = nlohmann::json::array();
    for (const auto & access : slot_accesses)
        result["slot_accesses"].push_back({
            {    "pool",                      access.pool },
            {   "field",                     access.field },
            {  "access", state_access_name(access.access) },
            { "members",                   access.members },
            {   "start",          access.start.document() },
            {  "finish",         access.finish.document() }
        });
    result["kv_accesses"] = nlohmann::json::array();
    for (const auto & endpoint : exports) result["exports"].push_back(endpoint.document());
    for (const auto & access : kv_accesses)
        result["kv_accesses"].push_back({
            {   "pool",                      access.pool },
            { "access", state_access_name(access.access) },
            {  "slice",          access.slice.document() },
            {   "rule",           access.rule.document() },
            {  "start",          access.start.document() },
            { "finish",         access.finish.document() }
        });
    for (const auto & resource : resources)
        result["resources"].push_back({
            {   "kind", resource_name(resource.kind) },
            {   "role",                resource.role },
            { "serial",              resource.serial }
        });
    for (const auto & piece : work)
        result["work"].push_back({
            { "resource",          piece.resource },
            {     "kind",   work_name(piece.kind) },
            {    "start",  piece.start.document() },
            {   "finish", piece.finish.document() }
        });
    for (const auto & edge : edges)
        result["edges"].push_back({
            { "source",   edge.source.document() },
            { "target",   edge.target.document() },
            {   "kind", relation_name(edge.kind) }
        });

    for (const auto & port : ports) result["ports"].push_back(port.document());
    for (const auto & child : children) {
        nlohmann::json arguments = nlohmann::json::object();
        for (const auto & [name, expression] : child.arguments) arguments[name] = expression.document();
        auto imports_json = nlohmann::json::array();
        for (const auto & imported : child.imports) imports_json.push_back(imported.document());
        result["children"].push_back({
            {  "contract", child.contract.document() },
            { "arguments",      std::move(arguments) },
            {   "imports",   std::move(imports_json) }
        });
    }
    return result;
}

TemplateFrame TemplateFrame::read(const nlohmann::json & source) {
    TemplateFrame result;
    result.definition = index(source.at("definition"));
    result.guard = Expression::read(source.at("guard"));
    if (source.contains("communication")) result.communication = CommunicationGroup::read(source.at("communication"));
    result.site = source.at("site").get<std::string>();
    result.bindings = source.at("bindings");
    result.constants = read_parameters(source.at("constants"));
    result.resources = indices(source.at("resources"));
    for (const auto & layer : source.at("kv_layers").get_ref<const nlohmann::json::array_t &>()) {
        if (!layer.is_number_integer()) throw std::invalid_argument("KV layer must be an integer");
        result.kv_layers.push_back(layer.get<std::int64_t>());
    }
    for (const auto & tensor : source.at("tensors").get_ref<const nlohmann::json::array_t &>()) {
        if (tensor.is_null()) result.tensors.push_back(std::nullopt);
        else {
            const auto version = index(tensor.at("version"));
            result.tensors.push_back(TensorBinding{ tensor.at("value").get<std::string>(),
                                                    tensor.at("storage").get<std::string>(),
                                                    static_cast<std::int64_t>(version),
                                                    tensor.at("external").get<bool>(),
                                                    std::nullopt,
                                                    tensor.at("buffer").get<std::string>() });
            if (!tensor.at("writer").is_null()) result.tensors.back()->writer = indices(tensor.at("writer"));
        }
    }
    for (const auto & child : source.at("children").get_ref<const nlohmann::json::array_t &>()) result.children.push_back(read(child));
    return result;
}

nlohmann::json TemplateFrame::local_document() const {
    nlohmann::json result{
        { "definition",              definition },
        {      "guard",        guard.document() },
        {       "site",                    site },
        {   "bindings",                bindings },
        {  "constants",               constants },
        {  "resources",               resources },
        {    "tensors", nlohmann::json::array() },
        {  "kv_layers",               kv_layers }
    };
    for (const auto & tensor : tensors) {
        if (tensor)
            result["tensors"].push_back({
                {    "value",                                                              tensor->value },
                {  "storage",                                                            tensor->storage },
                {  "version",                                                            tensor->version },
                { "external",                                                           tensor->external },
                {   "buffer",                                                             tensor->buffer },
                {   "writer", tensor->writer ? nlohmann::json(*tensor->writer) : nlohmann::json(nullptr) }
            });
        else result["tensors"].push_back(nullptr);
    }
    if (communication) result["communication"] = communication->document();
    return result;
}

nlohmann::json TemplateFrame::document() const {
    auto result = local_document();
    result["children"] = nlohmann::json::array();
    for (const auto & child : children) result["children"].push_back(child.document());
    return result;
}

TemplateLibrary TemplateLibrary::read(const nlohmann::json & source) {
    TemplateLibrary result;
    result.domain = source.at("domain");
    result.local_domain = source.at("local_domain");
    for (const auto & resource : source.at("resources").get_ref<const nlohmann::json::array_t &>()) result.resources.push_back(Resource::read(resource));
    for (const auto & definition : source.at("definitions").get_ref<const nlohmann::json::array_t &>())
        result.definitions.push_back(TemplateDefinition::read(definition));
    for (const auto & entry : source.at("entries").get_ref<const nlohmann::json::array_t &>()) {
        TemplateEntry value{ entry.at("action").get<std::string>(), entry.at("condition"), TemplateFrame::read(entry.at("frame")), {} };
        for (const auto & observation : entry.at("observations").get_ref<const nlohmann::json::array_t &>())
            value.observations.push_back({ index(observation.at("scope")), read_parameters(observation.at("parameters")) });
        result.entries.push_back(std::move(value));
    }
    for (const auto & local : source.at("implementations"))
        result.implementations.push_back({ local.at("contract"), index(local.at("definition")), Expression::read(local.at("guard")) });
    result.validate();
    return result;
}

nlohmann::json TemplateLibrary::document() const {
    nlohmann::json result{
        {      "domain",                  domain },
        { "local_domain",           local_domain },
        { "implementations", nlohmann::json::array() },
        {   "resources", nlohmann::json::array() },
        { "definitions", nlohmann::json::array() },
        {     "entries", nlohmann::json::array() }
    };
    for (const auto & resource : resources) result["resources"].push_back(resource.document());
    for (const auto & definition : definitions) result["definitions"].push_back(definition.document());
    for (const auto & local : implementations)
        result["implementations"].push_back({ { "contract", local.contract }, { "definition", local.definition }, { "guard", local.guard.document() } });
    for (const auto & entry : entries) {
        auto observations = nlohmann::json::array();
        for (const auto & observed : entry.observations)
            observations.push_back({
                {      "scope",      observed.scope },
                { "parameters", observed.parameters }
            });
        result["entries"].push_back({
            {       "action",            entry.action },
            {    "condition",         entry.condition },
            {        "frame",  entry.frame.document() },
            { "observations", std::move(observations) }
        });
    }
    return result;
}


} // namespace markov
