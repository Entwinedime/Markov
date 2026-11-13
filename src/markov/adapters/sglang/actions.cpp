#include "markov/adapters/sglang/actions.hpp"
#include "markov/adapters/sglang/templates.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace markov::sglang {

NativeActionGraphs::NativeActionGraphs(const TemplateLibrary & library, nlohmann::json environment, std::vector<Resource> resources, std::string group,
                                       std::string instance)
    : library_(library),
      selection_(library),
      environment_(std::move(environment)),
      resources_(std::move(resources)),
      worker_(library_, environment_),
      group_(std::move(group)),
      instance_(std::move(instance)) {}

void NativeActionGraphs::bind_inputs(const TemplateFrame & frame, const std::map<std::string, StorageBinding> & fields,
                                     const std::set<std::string> & explicit_inputs, GraphInvocation & call) const {
    const auto & body = library_.definitions[frame.definition];
    for (std::size_t port = 0; port < frame.tensors.size(); ++port) {
        const auto & tensor = frame.tensors[port];
        if (body.ports[port].use != PortUse::Data || !tensor || !tensor->external || tensor->writer) continue;

        // An explicit boundary value also binds every child view of that same source storage.
        if (explicit_inputs.contains(tensor->storage)) continue;
        const auto & name = body.ports[port].name;
        const auto found = fields.find(name);
        if (found == fields.end()) throw std::invalid_argument("native batch has no preceding value for " + frame.site + ":" + name);
        auto binding = found->second;
        binding.version -= tensor->version;
        if (binding.version < 0) throw std::invalid_argument("native batch tensor version precedes its template input");
        const auto [previous, inserted] = call.storage.emplace(tensor->storage, binding);
        if (!inserted && (previous->second.identity != binding.identity || previous->second.version != binding.version))
            throw std::invalid_argument("native batch aliases disagree at " + name);
    }
    for (std::size_t child = 0; child < frame.children.size(); ++child) bind_inputs(frame.children[child], fields, explicit_inputs, call);
}

std::set<std::string> NativeActionGraphs::bind_values(const TemplateEntry & entry, const nlohmann::json & values, Direction direction,
                                                      GraphInvocation & call) const {
    const auto & ports = library_.definitions[entry.frame.definition].ports;
    std::set<std::string> bound;
    for (const auto & [name, value] : values.get_ref<const nlohmann::json::object_t &>()) {
        const auto port = std::ranges::find_if(ports, [&](const auto & item) { return item.name == name && item.direction == direction; });
        if (port == ports.end() || !port->tensor
            || (port->use != PortUse::Data && !(direction == Direction::Input && port->use == PortUse::Metadata)))
            throw std::invalid_argument("native binding needs a tensor data port or metadata input: " + name);

        // A forwarded handle binds child views without making the caller an early reader of their contents.
        const auto & tensor = *entry.frame.tensors[static_cast<std::size_t>(port - ports.begin())];
        StorageBinding binding{ value.at("identity").get<std::string>(), value.at("version").get<std::int64_t>() };
        if (!tensor.external || tensor.writer.has_value() != (direction == Direction::Output) || !values_.contains(binding.identity) || binding.version < 0)
            throw std::invalid_argument("native value cannot bind the template boundary: " + name);
        if (direction == Direction::Input) {
            binding.version -= tensor.version;
            if (binding.version < 0) throw std::invalid_argument("native value precedes the template input: " + name);
        }
        const auto [previous, inserted] = call.storage.emplace(tensor.storage, binding);
        if (!inserted && (previous->second.identity != binding.identity || previous->second.version != binding.version))
            throw std::invalid_argument("native input aliases disagree at " + name);
        bound.insert(tensor.storage);
    }
    return bound;
}

void NativeActionGraphs::bind_buffers(const TemplateFrame & frame, GraphInvocation & call) {
    std::map<std::string, std::pair<std::string, std::int64_t>> storages;
    std::vector<const TemplateFrame *> frames{ &frame };
    for (std::size_t index = 0; index < frames.size(); ++index) {
        const auto & current = *frames[index];
        const auto & body = library_.definitions[current.definition];
        for (const auto & child : current.children) frames.push_back(&child);
        for (std::size_t port = 0; port < current.tensors.size(); ++port) {
            const auto & tensor = current.tensors[port];
            if (body.ports[port].use != PortUse::Data || !tensor || !tensor->external || call.storage.contains(tensor->storage)) continue;
            if (!tensor->writer) throw std::logic_error("unbound external input after batch binding");
            if (tensor->buffer.empty())
                throw std::invalid_argument("external writable tensor has no declared storage owner: " + current.site + ":" + body.ports[port].name);
            auto & storage = storages.try_emplace(tensor->storage, tensor->buffer, 0).first->second;
            storage.second = std::max(storage.second, tensor->version);
        }
    }

    // One native owner binds all views; reserve every mutation epoch before the next invocation reuses it.
    for (const auto & [storage, layout] : storages) {
        const auto & [name, last_version] = layout;
        auto & buffer = buffers_.try_emplace(name, StorageBinding{ group_ + "/buffer/" + name, 0 }).first->second;
        if (last_version >= std::numeric_limits<std::int64_t>::max() - buffer.version)
            throw std::overflow_error("persistent buffer version exceeds int64");
        call.storage.emplace(storage, buffer);
        buffer.version += last_version + 1;
    }
}

TemplateEntry NativeActionGraphs::select(std::string_view action, const nlohmann::json & condition, const Parameters & parameters) const {
    auto entry = selection_.select(action, NativeTemplates::condition(action, condition), parameters);
    const auto & expected = library_.definitions[entry.frame.definition].parameters;

    // Native descriptions include fixed parameters too; the shared selector already checks names and values.
    if (parameters.size() != expected.size()) throw std::invalid_argument("native action parameters differ from its template declaration");
    return entry;
}

std::map<std::string, TensorLayout> NativeActionGraphs::outputs(std::string_view action, const nlohmann::json & condition,
                                                                const Parameters & parameters) const {
    const auto & entry = select(action, condition, parameters);
    std::map<std::string, TensorLayout> result;
    for (const auto & port : library_.definitions[entry.frame.definition].ports) {
        if (port.use == PortUse::Data && port.direction == Direction::Output) result.emplace(port.name, port.tensor->evaluate(parameters));
    }
    return result;
}

CoarseGraph NativeActionGraphs::build(const ServingAction & action) {
    if (action.task.group != group_) throw std::invalid_argument("action sent to another participant's graph provider");
    auto entry = select(action.name, action.condition, action.parameters);
    worker_.bind(entry.frame, instance_, action.work);
    const auto live = action.work.at("live_batches").get<std::set<std::size_t>>();
    std::erase_if(batches_, [&](const auto & batch) { return !live.contains(batch.first); });
    GraphInvocation call{ action.name,  entry.condition, action.parameters,
                          resources_,   action.work,     group_ + "/action/" + std::to_string(action.task.sequence),
                          environment_, library_.domain, {} };
    call.pools.emplace("local", group_);
    if (action.work.contains("pools")) {
        for (const auto & [name, owner] : action.work.at("pools").get<std::map<std::string, std::string>>()) {
            if (!call.pools.emplace(name, owner).second) throw std::invalid_argument("native action overrides its local KV pool");
        }
    }
    signals_.clear();
    if (action.work.contains("live_signals")) { signals_ = action.work.at("live_signals").get<std::set<std::string>>(); }
    if (action.work.contains("signals")) {
        for (const auto & [name, signal] : action.work.at("signals").get<std::map<std::string, std::string>>()) {
            if (!signals_.contains(signal)) throw std::invalid_argument("native action refers to a released completion signal");
            call.signals.emplace(name, signal);
        }
    }
    for (const auto & [name, _] : entry.frame.constants) call.parameters.erase(name);
    values_ = action.work.at("live_values").get<std::set<std::string>>();
    const auto explicit_inputs = bind_values(entry, action.work.at("values"), Direction::Input, call);
    bind_values(entry, action.work.at("allocations"), Direction::Output, call);
    const auto & batch = action.work.at("batch");
    const std::map<std::string, StorageBinding> empty;
    const auto & fields = batch.is_null() ? empty : batches_[batch.get<std::size_t>()];
    bind_inputs(entry.frame, fields, explicit_inputs, call);
    bind_buffers(entry.frame, call);
    auto graph = library_.instantiate(entry, call);
    if (action.name == "query") graph.scopes.front().port("caller.return", Direction::Output);
    for (const auto & port : graph.scopes.front().ports) {
        if (port.use != PortUse::Data || port.direction != Direction::Output) continue;
        const auto & tensor = *port.tensor;
        values_.insert(tensor.storage);
        if (!batch.is_null()) batches_.at(batch.get<std::size_t>()).insert_or_assign(port.name, StorageBinding{ tensor.storage, tensor.version });
    }
    return graph;
}

std::set<std::string> NativeActionGraphs::retained_bindings() const {
    auto result = signals_;
    result.insert(values_.begin(), values_.end());
    for (const auto & [_, fields] : batches_) {
        for (const auto & [name, value] : fields) result.insert(value.identity);
    }
    for (const auto & [_, value] : buffers_) result.insert(value.identity);
    return result;
}

} // namespace markov::sglang
