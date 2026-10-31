#include "markov/adapters/frameworks/sglang/batches.hpp"

#include <algorithm>
#include <utility>

namespace markov::sglang {

BatchViews::BatchViews(TargetGraph & graph, const TemplateAsset & asset)
    : graph_(graph),
      prefill_only_(asset.environment().at("implementation").at("disaggregation_mode") == "prefill") {
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
    if (prefill_only_) return 0;

    // Native admission precedes Decode filtering, while the preceding Prefill is filtered first.
    return (running_ ? running_->requests.size() : 0) + (prefill_ ? prefill_->kept_rows.size() : 0);
}

void BatchViews::advance(Phase phase) {

    // Explicit native selection already performed this iteration's filtering and joining.
    if (!selected_) join_prefill(phase);
    if (phase == Phase::Decode && !selected_) filter(running_.value(), "decode");
    selected_ = false;
}

std::optional<std::size_t> BatchViews::join_prefill(Phase phase) {
    const std::string name = phase_name(phase);
    std::optional<std::size_t> completion;

    // Native scheduling filters the preceding Prefill before joining it to the running view.
    if (prefill_) {
        completion = filter(*prefill_, name);
        if (!prefill_only_ && !prefill_->requests.empty()) {
            if (running_) completion = merge(*running_, *prefill_, name);
            else running_ = std::move(prefill_);
        }
        prefill_.reset();
    }
    return completion;
}

void BatchViews::consume_running() {
    running_.reset();
    selected_ = false;
}

std::size_t BatchViews::select(const BatchSelection & selection) {
    if (selection.view == BatchView::Running) join_prefill(Phase::Decode);
    auto & view = (selection.view == BatchView::Previous ? prefill_ : running_).value();
    view.kept_rows.clear();
    for (const auto request : selection.requests) {
        const auto row = std::ranges::find(view.requests, request);
        if (row == view.requests.end()) throw LayoutMismatch("selected request has no retained batch column");
        view.kept_rows.push_back(row - view.requests.begin());
    }
    if (selection.view == BatchView::Previous && !prefill_only_) return join_prefill(Phase::Prefill).value();

    // PD can revisit the same previous batch while its transfer is pending; keep the surviving columns.
    const auto filtered = filter(view, selection.view == BatchView::Previous ? "prefill" : "decode");
    selected_ = true;
    if (view.requests.empty()) (selection.view == BatchView::Previous ? prefill_ : running_).reset();
    return filtered;
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
    const bool finished_prefill = prefill_ && !selected_ && (prefill_only_ || prefill_->kept_rows.empty());
    const bool finished_running = running_ && running_->kept_rows.empty();
    if (!finished_prefill && !finished_running) return std::nullopt;

    const bool owns_call = !graph_.composition();
    const auto region = owns_call ? graph_.begin_composition("Schedule", "control", {}, {}) : graph_.composition().value();

    // Native idle scheduling filters the last Prefill before clearing the running view.
    for (auto * view : { &prefill_, &running_ }) {
        if (*view && (prefill_only_ || view->value().kept_rows.empty())) {
            filter(view->value(), "control");
            view->reset();
        }
    }
    if (owns_call) graph_.end_composition({});
    return region;
}

void BatchViews::remember(Phase phase, const std::map<std::string, std::size_t> & ports, const BatchEffects & effects) {
    View view{ columns(ports), {}, {} };
    for (std::size_t row = 0; row < effects.members.size(); ++row) {
        view.requests.push_back(effects.members[row].request);
        const auto & member = effects.members[row];
        if (!member.finished && !member.continues_prefill) view.kept_rows.push_back(static_cast<std::int64_t>(row));
    }

    (phase == Phase::Decode ? running_ : prefill_) = std::move(view);
}

void BatchViews::live_values(std::vector<std::size_t *> & values) {
    for (auto * view : { &running_, &prefill_ }) {
        if (*view) {
            for (auto & [_, value] : view->value().columns) values.push_back(&value);
        }
    }
}

std::size_t BatchViews::filter(View & view, const std::string & phase) {
    Invocation invocation;
    invocation.parameters = {
        { "before_members",  static_cast<std::int64_t>(view.requests.size()) },
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
        if (view.kept_rows.size() != view.requests.size()) {
            IntegerData rows;
            for (const auto row : view.kept_rows) rows.append(row);
            graph_.bind_integers(filtered.ports.outputs.at("kept_rows"), std::move(rows));
        }
        view.columns = outputs;
    }
    else view.columns.clear();

    std::vector<std::size_t> selected;
    for (const auto row : view.kept_rows) selected.push_back(view.requests.at(static_cast<std::size_t>(row)));
    view.requests = std::move(selected);
    for (std::size_t row = 0; row < view.kept_rows.size(); ++row) view.kept_rows[row] = static_cast<std::int64_t>(row);
    return filtered.region;
}

std::size_t BatchViews::merge(View & left, const View & right, const std::string & phase) {
    Invocation invocation;
    invocation.parameters = {
        { "before_members",  static_cast<std::int64_t>(left.requests.size()) },
        {  "added_members", static_cast<std::int64_t>(right.requests.size()) }
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

    for (const auto row : right.kept_rows) left.kept_rows.push_back(static_cast<std::int64_t>(left.requests.size()) + row);
    left.requests.insert(left.requests.end(), right.requests.begin(), right.requests.end());
    left.columns = std::move(outputs);
    return merged.region;
}

} // namespace markov::sglang
