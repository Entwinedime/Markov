#pragma once

#include "markov/adapters/frameworks/sglang/batch_layout.hpp"
#include "markov/graph/source_graph.hpp"

#include <nlohmann/json_fwd.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>

namespace markov {

struct TemplateDefinition;

// Native tensor provenance is consumed once at production. Runtime templates
// retain only the qualified extent and element size, never profiler records.
class SGLangCopyLayout {
public:
    SGLangCopyLayout(const nlohmann::json & graph, const nlohmann::json & facts);
    void bind(TemplateDefinition & definition, const nlohmann::json & calls, const std::map<std::size_t, NativeFilterSelection> & filters) const;

private:
    void bind_input_transfers(TemplateDefinition & definition) const;

    struct Tensor {
        std::uint64_t elements;
        std::uint64_t element_bytes;
        std::string dtype;
    };

    std::map<SourceSpanId, Tensor> tensors_;
    std::set<SourceSpanId> input_transfers_;
};

} // namespace markov
