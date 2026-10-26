#include "markov/adapters/frameworks/sglang/batches.hpp"

#include <utility>

namespace markov::sglang {

BatchViews::BatchViews(TargetGraph & graph, const TemplateAsset & asset) : graph_(graph) {
    for (const auto & entry : asset.entrypoints()) {
        if (entry.role != "batch_filter") continue;
        for (const auto & [name, _] : asset.layout(asset.frame(entry).layout).ports.inputs) fields_.insert(name);
    }
}

std::map<std::string, std::size_t> BatchViews::columns(const std::map<std::string, std::size_t> & ports) const {
    std::map<std::string, std::size_t> result;
    for (const auto & name : fields_) {
        if (const auto found = ports.find(name); found != ports.end()) result.emplace(*found);
    }

    return result;
}

std::size_t BatchViews::admission_members() const {

    // Native admission precedes Decode filtering, while the preceding Prefill is filtered first.
    return (running_ ? static_cast<std::size_t>(running_->members) : 0) + (prefill_ ? prefill_->kept_rows.size() : 0);
}

void BatchViews::advance(Phase phase) {
    const std::string name = phase == Phase::Prefill ? "prefill" : "decode";

    // Native scheduling filters the preceding Prefill before joining it to the running view.
    if (prefill_) {
        filter(*prefill_, name);
        if (prefill_->members != 0) {
            if (running_) merge(*running_, *prefill_, name);
            else running_ = std::move(prefill_);
        }
        prefill_.reset();
    }

    if (phase == Phase::Decode) filter(running_.value(), name);
}

void BatchViews::accept() {
    advance(Phase::Prefill);

    // Destination admission removes finished rows before attaching the next prebuilt batch.
    if (running_) {
        filter(*running_, "control");
        if (running_->members == 0) running_.reset();
    }
}

std::map<std::string, std::size_t> BatchViews::decode_inputs() const {
    auto inputs = running_.value().columns;
    inputs["prefix_lengths"] = inputs.at("sequence_lengths");
    inputs["prefix_lengths_cpu"] = inputs.at("sequence_lengths_cpu");
    inputs.erase("sequence_lengths");
    inputs.erase("sequence_lengths_cpu");
    return inputs;
}

std::optional<std::size_t> BatchViews::idle() {
    const bool finished_prefill = prefill_ && prefill_->kept_rows.empty();
    const bool finished_running = running_ && running_->kept_rows.empty();
    if (!finished_prefill && !finished_running) return std::nullopt;

    const auto region = graph_.begin_composition("Schedule", "control", {}, {});

    // Native idle scheduling filters the last Prefill before clearing the running view.
    for (auto * view : { &prefill_, &running_ }) {
        if (*view && view->value().kept_rows.empty()) {
            filter(view->value(), "control");
            view->reset();
        }
    }
    graph_.end_composition({});
    return region;
}

void BatchViews::remember(Phase phase, const std::map<std::string, std::size_t> & ports, const BatchEffects & effects) {
    View view{ columns(ports), static_cast<std::int64_t>(effects.members.size()), {} };
    for (std::size_t row = 0; row < effects.members.size(); ++row) {
        if (!effects.members[row].finished) view.kept_rows.push_back(static_cast<std::int64_t>(row));
    }

    (phase == Phase::Prefill ? prefill_ : running_) = std::move(view);
}

void BatchViews::live_values(std::vector<std::size_t *> & values) {
    for (auto * view : { &running_, &prefill_ }) {
        if (*view) {
            for (auto & [_, value] : view->value().columns) values.push_back(&value);
        }
    }
}

void BatchViews::filter(View & view, const std::string & phase) {
    Invocation invocation;
    invocation.parameters = {
        { "before_members",                                     view.members },
        {  "after_members", static_cast<std::int64_t>(view.kept_rows.size()) }
    };
    const auto filtered = graph_.append("batch_filter", phase, invocation, view.columns);

    if (!view.kept_rows.empty()) {
        const auto outputs = columns(filtered.ports.outputs);
        for (const auto & [name, value] : outputs) {
            const auto known = graph_.graph().integer_data.find(view.columns.at(name));
            if (known == graph_.graph().integer_data.end()) continue;

            IntegerData selected;
            for (const auto row : view.kept_rows) selected.append(known->second.slice(row, 1));
            graph_.bind_integers(value, std::move(selected));
        }

        // Selection contents annotate the observed index tensor without removing its native work.
        if (view.kept_rows.size() != static_cast<std::size_t>(view.members)) {
            IntegerData rows;
            for (const auto row : view.kept_rows) rows.append(row);
            graph_.bind_integers(filtered.ports.outputs.at("kept_rows"), std::move(rows));
        }
        view.columns = outputs;
    }
    else view.columns.clear();

    view.members = static_cast<std::int64_t>(view.kept_rows.size());
    for (std::size_t row = 0; row < view.kept_rows.size(); ++row) view.kept_rows[row] = static_cast<std::int64_t>(row);
}

void BatchViews::merge(View & left, const View & right, const std::string & phase) {
    Invocation invocation;
    invocation.parameters = {
        { "before_members",  left.members },
        {  "added_members", right.members }
    };
    std::map<std::string, std::size_t> inputs;
    for (const auto & [name, value] : left.columns) inputs.emplace("left." + name, value);
    for (const auto & [name, value] : right.columns) inputs.emplace("right." + name, value);
    const auto merged = graph_.append("batch_merge", phase, invocation, inputs);
    auto outputs = columns(merged.ports.outputs);

    for (const auto & [name, value] : outputs) {
        const auto & contents = graph_.graph().integer_data;
        const auto first = contents.find(left.columns.at(name));
        const auto second = contents.find(right.columns.at(name));
        if (first == contents.end() || second == contents.end()) continue;

        auto joined = first->second;
        joined.append(second->second);
        graph_.bind_integers(value, std::move(joined));
    }

    for (const auto row : right.kept_rows) left.kept_rows.push_back(left.members + row);
    left.members += right.members;
    left.columns = std::move(outputs);
}

} // namespace markov::sglang
