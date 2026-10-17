#pragma once

#include "markov/templates/communication_work.hpp"

#include <nlohmann/json_fwd.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <vector>

namespace markov {

struct TemplateSource;
struct TemplateDefinition;
class RequestPoolLayout;
class SGLangCopyLayout;
class ModelParameterRule;
class AttentionParameterRule;

// Produce an owning template source and interpret native scopes during that
// construction. Consumers receive layouts, dependencies and operation identity.
class SGLangTemplateBindings {
public:
    // Qualified contiguous INT64 scalar-list branch of ACLNN ConcatD.
    static constexpr std::uint64_t last_location_concat_limit = 32;
    static TemplateSource produce(const std::filesystem::path & manifest_path);

    SGLangTemplateBindings(TemplateDefinition & definition, std::set<ModelParameterRule> & rules, std::set<CollectiveParameterRule> & collectives,
                           std::set<AttentionParameterRule> & attention)
        : definition_(definition),
          collective_rules_(collectives),
          attention_rules_(attention),
          model_rules_(rules) {}
    void bind_membership(const nlohmann::json & source_state);
    void bind_publication(const nlohmann::json * published_requests, const nlohmann::json & source_state);
    void bind_kv(std::uint32_t page_size_tokens);
    void bind_length_domain(std::uint32_t page_size_tokens, std::optional<std::uint32_t> sequence_capacity_tokens);
    void bind_request_pool(const RequestPoolLayout & pool);
    void bind(const nlohmann::json & graph, const nlohmann::json & regions, const nlohmann::json & calls, const SGLangCopyLayout & copies,
              std::uint32_t page_size_tokens);

private:
    void bind_regions(const nlohmann::json & regions);
    void bind_admission_work();
    void bind_transition_merge();
    void bind_publication_progress();
    void bind_publication_output();
    void bind_layouts(const nlohmann::json & graph, const nlohmann::json & calls, std::uint32_t page_size_tokens);
    void bind_last_locations(std::uint32_t page_size_tokens);
    void bind_request_initialization();
    void bind_decode_locations();
    void bind_release_transfer(std::uint64_t sequence_tokens);
    void bind_prefix_publication();
    void bind_rotary_positions();
    std::map<std::size_t, CollectiveCountRule> bind_collectives() const;
    void bind_logits_reshape();
    std::optional<std::size_t> logits_gather_reshape() const;

    TemplateDefinition & definition_; // Borrowed only while producing one definition.
    std::set<CollectiveParameterRule> & collective_rules_;
    std::set<AttentionParameterRule> & attention_rules_;
    std::set<ModelParameterRule> & model_rules_; // Owned by the produced source.
};

} // namespace markov
