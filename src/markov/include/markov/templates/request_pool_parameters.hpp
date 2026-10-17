#pragma once

#include "markov/templates/tensor_parameters.hpp"

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <utility>

namespace markov {

struct RequestPoolCapacityDomain {
    std::uint32_t maximum;
    std::optional<std::string> shrink_gap;

    std::optional<std::string> unsupported(std::uint32_t capacity) const;
};

class RequestPoolParameterLayout {
    friend class TemplateAssetCodec;

public:
    RequestPoolParameterLayout(TensorParameterLayout shapes, std::optional<RequestPoolOperation> access, bool single_request_only = false)
        : shapes_(std::move(shapes)),
          access_(access),
          single_request_only_(single_request_only) {}
    std::expected<SourceKernel, std::string> evaluate(const SourceKernel & source, const BatchInput & input, std::uint32_t capacity) const;
    nlohmann::json describe_parameters() const;

private:
    TensorParameterLayout shapes_;
    std::optional<RequestPoolOperation> access_;
    bool single_request_only_;
};

} // namespace markov
