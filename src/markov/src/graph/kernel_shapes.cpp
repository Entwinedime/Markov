#include "markov/graph/kernel_shapes.hpp"

#include <charconv>
#include <stdexcept>
#include <utility>

namespace markov {

KernelShapes::KernelShapes(std::string_view text) {
    if (text.starts_with('"') && text.ends_with('"')) text = text.substr(1, text.size() - 2);
    // Empty slots are omitted optional operands, not scalar dimensions.
    while (true) {
        const auto separator = text.find(';');
        auto tensor = text.substr(0, separator);
        std::vector<std::int64_t> shape;
        while (!tensor.empty()) {
            const auto comma = tensor.find(',');
            const auto dimension = tensor.substr(0, comma);
            std::int64_t value;
            const auto parsed = std::from_chars(dimension.data(), dimension.data() + dimension.size(), value);
            if (parsed.ec != std::errc{} || parsed.ptr != dimension.data() + dimension.size()) {
                throw std::invalid_argument("invalid native kernel tensor dimension");
            }
            shape.push_back(value);
            if (comma == std::string_view::npos) break;
            tensor.remove_prefix(comma + 1);
        }
        tensors.push_back(std::move(shape));
        if (separator == std::string_view::npos) break;
        text.remove_prefix(separator + 1);
    }
}

std::string KernelShapes::encode() const {
    std::string result = "\"";
    for (std::size_t slot = 0; slot < tensors.size(); ++slot) {
        if (slot != 0) result += ';';
        for (std::size_t axis = 0; axis < tensors[slot].size(); ++axis) {
            if (axis != 0) result += ',';
            result += std::to_string(tensors[slot][axis]);
        }
    }
    return result + '"';
}

} // namespace markov
