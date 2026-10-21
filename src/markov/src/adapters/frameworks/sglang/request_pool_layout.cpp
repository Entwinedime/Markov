#include "markov/adapters/frameworks/sglang/request_pool_layout.hpp"

#include "markov/contracts/source_observations.hpp"
#include "markov/graph/kernel_shapes.hpp"
#include "markov/graph/source_graph.hpp"

#include <algorithm>
#include <limits>

#include <utility>
#include <vector>

namespace markov {
RequestPoolLayout::RequestPoolLayout(std::uint32_t source_capacity, const std::map<SourceSpanId, SourceKernel> & kernels,
                                     const std::map<SourceSpanId, SourceOperation> & operations)
    : source_capacity_(source_capacity) {
    for (const auto & [span, kernel] : kernels) {
        if (kernel.name != "write_req_to_token_pool_triton") continue;
        const auto shapes = KernelShapes(kernel.output_shapes);
        if (shapes.tensors.size() != 1) {
            table_.reset();
            return;
        }
        const auto & shape = shapes.tensors.front();
        if (shape.size() != 2 || shape[0] != static_cast<std::int64_t>(source_capacity_) + 1 || shape[1] <= 0 || (table_ && (*table_)[1] != shape[1])) {
            table_.reset();
            return;
        }
        table_ = std::array{ shape[0], shape[1] };
    }

    // Pair by recorded operation membership, then interpret the root assignment.
    for (const auto & [span, operation] : operations) {
        if (operation.name != "aten::index_put_" || !operation.input_shapes || !operation.input_types) continue;
        const auto & members = operation.device_spans;
        if (members.empty() || members.size() > 2) continue;
        const SourceKernel * write = nullptr;
        SourceSpanId write_span = 0;
        std::optional<SourceSpanId> broadcast;
        for (const auto span : members) {
            const auto found = kernels.find(span);
            if (found == kernels.end()) continue;
            if (found->second.name == "aclnnIndexPutImpl_IndexPutV2_IndexPutV2") {
                write = &found->second;
                write_span = span;
            }
            else if (found->second.name == "aclnnIndexPutImpl_BroadcastToAiCore_BroadcastTo") broadcast = span;
        }
        if (!write || members.size() != (broadcast ? 2 : 1) || !table_) continue;

        auto shapes = *operation.input_shapes;
        auto types = *operation.input_types;
        std::erase_if(shapes, [](char c) { return c == ' ' || c == '\r' || c == '\n' || c == '\t'; });
        std::erase_if(types, [](char c) { return c == ' ' || c == '\r' || c == '\n' || c == '\t'; });
        const KernelShapes root(shapes);
        const KernelShapes operands(write->input_shapes);
        if (root.tensors.size() != 4 || root.tensors[0] != operands.tensors[0] || !root.tensors[1].empty() || !root.tensors[3].empty()) continue;
        const bool scalar = root.tensors[2].empty();
        if (!scalar && (root.tensors[2].size() != 1 || root.tensors[2][0] <= 1)) continue;
        if (scalar != broadcast.has_value()) continue;

        BatchOperationRole destination;
        if (root.tensors[0] == std::vector<std::int64_t>{ (*table_)[0] } && types == "longint;None;longint;Scalar" && write->output_dtypes == "INT64")
            destination = BatchOperationRole::RelayWrite;
        else if (root.tensors[0] == std::vector<std::int64_t>{ (*table_)[0], (*table_)[1] } && types == "int;None;int;Scalar"
                 && write->output_dtypes == "INT32")
            destination = BatchOperationRole::RequestTableWrite;
        else continue;

        const auto host = operation.host_span;
        const auto requests = scalar ? 1 : static_cast<std::uint64_t>(root.tensors[2][0]);
        const BatchOperationVariant variant{ destination, requests, scalar ? 1U : 2U, scalar ? 1 : std::numeric_limits<std::uint64_t>::max() };
        indexed_writes_.emplace(host, IndexedWriteSource{ variant, write_span, write });
        if (broadcast) broadcasts_.emplace(*broadcast, host);
    }
}

const IndexedWriteSource * RequestPoolLayout::indexed_write(SourceSpanId operation) const {
    const auto found = indexed_writes_.find(operation);
    return found == indexed_writes_.end() ? nullptr : &found->second;
}

std::expected<RequestPoolParameterLayout, std::string> RequestPoolLayout::bind_broadcast(const SourceKernel & broadcast, const IndexedWriteSource & binding,
                                                                                         std::span<const SourceRequest> source_requests) const {
    const auto & write = *binding.write;
    if (source_requests.size() != 1) return std::unexpected("indexed_write_scalar_source_not_qualified");

    const auto & dtype = write.output_dtypes;

    const KernelShapes inputs(broadcast.input_shapes);
    const KernelShapes outputs(broadcast.output_shapes);
    if (inputs.tensors != std::vector<std::vector<std::int64_t>>{ { 1 }, { 1 } } || outputs.tensors != std::vector<std::vector<std::int64_t>>{ { 1 } }
        || broadcast.input_dtypes != dtype + ";INT64" || broadcast.output_dtypes != dtype || broadcast.input_formats != "ND;ND"
        || broadcast.output_formats != "ND")
        return std::unexpected("indexed_write_broadcast_layout_not_qualified");

    const auto paired = bind(binding.write_span, write, source_requests);
    if (!paired) return std::unexpected(paired.error());
    if (!*paired) return std::unexpected("indexed_write_destination_not_qualified");
    return RequestPoolParameterLayout(TensorParameterLayout({}, {}, 0), std::nullopt, true);
}

std::optional<std::int64_t> RequestPoolLayout::sequence_capacity() const {
    if (!table_) return std::nullopt;
    return (*table_)[1];
}

RequestPoolCapacityDomain RequestPoolLayout::capacity_domain() const {
    return { source_capacity_, table_ ? std::nullopt : std::optional<std::string>("request_pool_source_layout_unresolved") };
}

std::expected<std::optional<RequestPoolParameterLayout>, std::string> RequestPoolLayout::bind(SourceSpanId source_span, const SourceKernel & source,
                                                                                              std::span<const SourceRequest> source_requests) const {
    if (source.name == "aclnnIndexPutImpl_BroadcastToAiCore_BroadcastTo") {
        const auto binding = broadcasts_.find(source_span);
        if (binding == broadcasts_.end()) return std::unexpected("indexed_write_operation_not_observed");
        auto target = bind_broadcast(source, indexed_writes_.at(binding->second), source_requests);
        if (!target) return std::unexpected(target.error());
        return std::optional<RequestPoolParameterLayout>{ std::move(*target) };
    }

    const auto observed_batch_size = static_cast<std::int64_t>(source_requests.size());
    std::int64_t source_sequence_tokens = 0;
    for (const auto & request : source_requests)
        source_sequence_tokens = std::max(source_sequence_tokens, static_cast<std::int64_t>(request.context_tokens) + request.input_tokens);
    if (!table_) return std::unexpected("request_pool_source_layout_unresolved");

    const auto [rows, columns] = *table_;
    using Extent = TensorAxisExtent;
    std::vector<TensorParameterAxis> input_axes;
    std::vector<TensorParameterAxis> output_axes;
    std::optional<RequestPoolOperation> access;
    KernelShapes inputs(source.input_shapes);
    KernelShapes outputs(source.output_shapes);
    const bool index = source.name == "aclnnIndex_IndexAiCore_Index";
    const bool write = source.name == "aclnnIndexPutImpl_IndexPutV2_IndexPutV2";

    if ((index || write) && inputs.tensors.front() == std::vector<std::int64_t>{ rows }) {
        const auto dtype = index ? "INT64;INT64;INT64;INT64" : "INT64;INT64;INT64;INT64;INT64";
        if (source.output_dtypes != "INT64" || source.input_dtypes != dtype) return std::unexpected("request_pool_relay_dtype_unmapped");
        const std::vector<std::vector<std::int64_t>> expected_inputs =
            index ? std::vector<std::vector<std::int64_t>>{ { rows }, { 1 }, { 1 }, { observed_batch_size } }
                  : std::vector<std::vector<std::int64_t>>{ { rows }, { observed_batch_size }, { 1 }, { 1 }, { observed_batch_size } };
        const std::vector<std::vector<std::int64_t>> expected_outputs{ { index ? observed_batch_size : rows } };
        if (inputs.tensors != expected_inputs || outputs.tensors != expected_outputs) return std::unexpected("request_pool_relay_layout_unmapped");

        input_axes = {
            { 0, 1, Extent::RequestPoolRows }
        };
        if (index) input_axes.push_back({ 3, 1, Extent::Requests });
        else {
            input_axes.push_back({ 1, 1, Extent::Requests });
            input_axes.push_back({ 4, 1, Extent::Requests });
        }
        output_axes = {
            { 0, 1, index ? Extent::Requests : Extent::RequestPoolRows }
        };
        access = index ? RequestPoolOperation::RelayRead : RequestPoolOperation::RelayWrite;
    }
    else {
        bool touches_table = false;
        for (const auto & shape : inputs.tensors) touches_table = touches_table || (shape.size() == 2 && shape[0] == rows);
        for (const auto & shape : outputs.tensors) touches_table = touches_table || (shape.size() == 2 && shape[0] == rows);
        if (!touches_table) return std::optional<RequestPoolParameterLayout>{};
        if (source.output_dtypes != "INT32") return std::unexpected("request_pool_operand_dtype_unmapped");

        // Match complete operand roles before changing axes. Descriptor slots
        // of length 1/2 are constants even when B happens to equal them.
        std::vector<std::vector<std::int64_t>> expected_inputs;
        std::vector<std::vector<std::int64_t>> expected_outputs;
        if (source.name == "write_req_to_token_pool_triton") {
            expected_inputs = { { observed_batch_size }, { observed_batch_size }, { observed_batch_size }, { observed_batch_size } };
            expected_outputs = {
                { rows, columns }
            };
            if (source.input_dtypes != "INT64;UINT64;INT64;INT64") return std::unexpected("request_pool_operand_dtype_unmapped");
            if (inputs.tensors != expected_inputs || outputs.tensors != expected_outputs) return std::unexpected("request_pool_operand_layout_unmapped");
            // Tensor extents come from the vendor signature. Indexed rows and
            // write ranges come from the target reservations, not pointer data.
            for (std::size_t slot = 0; slot < inputs.tensors.size(); ++slot) input_axes.push_back({ slot, 1, Extent::Requests });
            output_axes = {
                { 0, 1, Extent::RequestPoolRows }
            };
            access = RequestPoolOperation::PrefixExtendWrite;
        }
        else if (source.name == "aclnnIndex_SliceAiCore_Slice") {
            expected_inputs = {
                { rows, columns },
                { 2 },
                { 2 }
            };
            expected_outputs = {
                { rows, source_sequence_tokens }
            };
            if (source.input_dtypes != "INT32;INT64;INT64") return std::unexpected("request_pool_operand_dtype_unmapped");
            if (inputs.tensors != expected_inputs || outputs.tensors != expected_outputs) return std::unexpected("request_pool_operand_layout_unmapped");
            input_axes = {
                { 0, 1, Extent::RequestPoolRows }
            };
            output_axes = {
                { 0, 1, Extent::RequestPoolRows },
                { 0, 1, Extent::SequenceTokens, 1 }
            };
        }
        else if (index && inputs.tensors.size() == 4) {
            expected_inputs = {
                { rows, source_sequence_tokens },
                { 1 },
                { 2 },
                { observed_batch_size }
            };
            expected_outputs = {
                { observed_batch_size, source_sequence_tokens }
            };
            if (source.input_dtypes != "INT32;INT64;INT64;INT64") return std::unexpected("request_pool_operand_dtype_unmapped");
            if (inputs.tensors != expected_inputs || outputs.tensors != expected_outputs) return std::unexpected("request_pool_operand_layout_unmapped");
            input_axes = {
                { 0, 1, Extent::RequestPoolRows },
                { 0, 1, Extent::SequenceTokens, 1 },
                { 3, 1, Extent::Requests }
            };
            output_axes = {
                { 0, 1, Extent::Requests },
                { 0, 1, Extent::SequenceTokens, 1 }
            };
            access = RequestPoolOperation::RowRead;
        }
        else if (index || write) {
            expected_inputs = index ? std::vector<std::vector<std::int64_t>>{ { rows, columns }, { 2 }, { 1 }, { observed_batch_size }, { observed_batch_size } }
                                    : std::vector<std::vector<std::int64_t>>{ { rows, columns }, { observed_batch_size }, { 2 }, { 2 }, { observed_batch_size }, { observed_batch_size } };
            expected_outputs = index ? std::vector<std::vector<std::int64_t>>{ { observed_batch_size } }
                                     : std::vector<std::vector<std::int64_t>>{ { rows, columns } };
            const auto dtype = index ? "INT32;INT64;INT64;INT64;INT64" : "INT32;INT32;INT64;INT64;INT64;INT64";
            if (source.input_dtypes != dtype) return std::unexpected("request_pool_operand_dtype_unmapped");
            if (inputs.tensors != expected_inputs || outputs.tensors != expected_outputs) return std::unexpected("request_pool_operand_layout_unmapped");
            input_axes = {
                { 0, 1, Extent::RequestPoolRows }
            };
            if (index) {
                input_axes.push_back({ 3, 1, Extent::Requests });
                input_axes.push_back({ 4, 1, Extent::Requests });
                output_axes = {
                    { 0, 1, Extent::Requests }
                };
            }
            else {
                input_axes.push_back({ 1, 1, Extent::Requests });
                input_axes.push_back({ 4, 1, Extent::Requests });
                input_axes.push_back({ 5, 1, Extent::Requests });
                output_axes = {
                    { 0, 1, Extent::RequestPoolRows }
                };
            }
            access = index ? RequestPoolOperation::TokenRead : RequestPoolOperation::TokenWrite;
        }
        else return std::unexpected("request_pool_operand_layout_unmapped");
    }

    return RequestPoolParameterLayout(TensorParameterLayout(std::move(input_axes), std::move(output_axes), 0), access);
}

} // namespace markov
