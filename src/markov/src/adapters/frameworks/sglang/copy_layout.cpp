#include "markov/adapters/frameworks/sglang/copy_layout.hpp"

#include "markov/graph/kernel_shapes.hpp"
#include "markov/templates/templates.hpp"
#include "markov/templates/transfer_parameters.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

namespace markov {
namespace {

std::string tensor_arguments(std::string text) {
    // The profiler exports both literal line breaks and escaped separators.
    for (const std::string separator : { "\\r\\n", "\r\n" }) {
        std::size_t position = 0;
        while ((position = text.find(separator, position)) != std::string::npos) text.erase(position, separator.size());
    }
    return text;
}

} // namespace

SGLangCopyLayout::SGLangCopyLayout(const nlohmann::json & graph, const nlohmann::json & facts) {
    const auto & intervals = facts.at("intervals");
    std::map<std::size_t, std::vector<std::size_t>> enqueues;
    for (const auto & flow : facts.at("flows")) {
        if (flow.at("category") != "async_task_queue") continue;
        for (const auto & target : flow.at("target_intervals")) {
            auto & sources = enqueues[target.get<std::size_t>()];
            for (const auto & source : flow.at("source_intervals")) sources.push_back(source.get<std::size_t>());
        }
    }

    using Thread = std::pair<std::uint32_t, std::uint64_t>;
    std::map<Thread, std::vector<const nlohmann::json *>> copies;
    for (const auto & interval : intervals) {
        if (interval.at("kind") == "cpu" && interval.at("name") == "aten::copy_")
            copies[{ interval.at("rank").get<std::uint32_t>(), interval.at("tid").get<std::uint64_t>() }].push_back(&interval);
    }

    for (const auto & transfer : graph.at("transfers")) {
        const auto & api = facts.at("native_api").at(transfer.at("source_api").get<std::size_t>());
        if (api.at("copy_execution") != "synchronous_driver" || transfer.at("direction") != "host_to_device") continue;
        const auto span = transfer.at("source_span").get<SourceSpanId>();
        const auto & source_interval = graph.at("spans").at(span).at("source_interval");
        if (source_interval.is_null()) continue;
        const auto worker_id = source_interval.get<std::size_t>();
        const auto & worker = intervals.at(worker_id);
        if (worker.at("name") != "Dequeue@acl_memcpy_host_to_device") continue;
        const auto hosts = enqueues.find(worker_id);
        if (hosts == enqueues.end() || hosts->second.size() != 1) continue;

        const auto & host = intervals.at(hosts->second.front());
        const Thread thread{ host.at("rank").get<std::uint32_t>(), host.at("tid").get<std::uint64_t>() };
        const nlohmann::json * operation = nullptr;
        bool ambiguous = false;
        for (const auto * candidate : copies[thread]) {
            if (candidate->at("start_ns") > host.at("start_ns") || candidate->at("end_ns") < host.at("end_ns")) continue;
            if (operation) {
                ambiguous = true;
                break;
            }
            operation = candidate;
        }
        if (!operation || ambiguous) continue;

        const auto & arguments = operation->at("arguments");
        const KernelShapes shapes(tensor_arguments(arguments.at("Input Dims").get<std::string>()));
        const auto types = tensor_arguments(arguments.at("Input type").get<std::string>());
        const auto separator = types.find(';');
        if (separator == std::string::npos || shapes.tensors.size() < 2 || shapes.tensors[0].size() != 1 || shapes.tensors[0] != shapes.tensors[1]
            || shapes.tensors[0][0] <= 0)
            continue;
        const auto dtype = types.substr(0, separator);
        const auto second_end = types.find(';', separator + 1);
        if (types.substr(separator + 1, second_end - separator - 1) != dtype) continue;

        std::uint64_t element_bytes = 0;
        if (dtype == "long int" || dtype == "long unsigned int") element_bytes = 8;
        else if (dtype == "int" || dtype == "float") element_bytes = 4;
        else continue;
        const auto elements = static_cast<std::uint64_t>(shapes.tensors[0][0]);
        if (elements * element_bytes != transfer.at("bytes").get<std::uint64_t>()) continue;
        tensors_.emplace(span, Tensor{ elements, element_bytes, dtype });
    }
}

void SGLangCopyLayout::bind(TemplateDefinition & definition, const nlohmann::json & calls, const std::map<std::size_t, NativeFilterSelection> & filters) const {
    const bool cold_prefill = definition.phase == BatchPhase::Prefill
                              && std::ranges::all_of(definition.source_requests, [](const auto & request) { return request.context_tokens == 0; });
    for (const auto & node : definition.graph) {
        const auto & observation = definition.observation(node);
        const auto found = tensors_.find(observation.source_span);
        if (found == tensors_.end() || !observation.source_scope) continue;
        const auto & tensor = found->second;
        const auto scope = *observation.source_scope;
        std::optional<TransferExtent> extent;

        // prepare_for_extend and its allocation/sampling setup construct
        // request vectors; forward metadata constructs two INT32 vectors.
        const auto & call = calls.at(scope);
        if (cold_prefill && tensor.elements == definition.source_requests.size()) {
            if (definition.step == StepKind::Prepare && scope == definition.source_call) extent = TransferExtent::RequestElements;
            if (definition.step == StepKind::Prefill && call.at("native_step") == "model_forward" && call.at("parent") == definition.source_call
                && tensor.dtype == "int")
                extent = TransferExtent::RequestElements;
        }

        const auto filter = filters.find(scope);
        if (filter != filters.end() && tensor.dtype == "long int") {
            const auto & membership = definition.filters.at(filter->second.index);
            if (tensor.elements == membership.retained_indices.size() && tensor.elements > 0 && tensor.elements < membership.input_count)
                extent = filter->second.role == BatchFilterBinding::Transition ? TransferExtent::TransitionRetainedRequests
                                                                               : TransferExtent::RunningRetainedRequests;
        }
        if (extent)
            definition.transfer_layouts.emplace(observation.source_span,
                                                TransferParameterLayout{ *extent, definition.source_requests.size(), 0, tensor.element_bytes });
    }
}

} // namespace markov
