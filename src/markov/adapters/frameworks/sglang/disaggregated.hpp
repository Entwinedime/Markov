#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <span>
#include <vector>

#include "markov/adapters/frameworks/sglang/kv_transfer.hpp"
#include "markov/adapters/frameworks/sglang/structure.hpp"
#include "markov/controller/disaggregated.hpp"
#include "markov/templates/batch_expansion.hpp"

namespace markov::sglang {

// Each instance translates its native calls; only explicit data-ready ports cross the PD boundary.
class DisaggregatedStructure final : public BatchExpansion {
public:
    DisaggregatedStructure(std::span<const TemplateAsset> assets, ServingSettings prefill, ServingSettings decode, std::span<const Request> requests,
                           const DisaggregatedGroups & groups, std::span<const std::int64_t> devices, const nlohmann::json & prefill_routes,
                           const nlohmann::json & decode_routes);

    GroupAdmission admission() const override;
    BatchProgram expand(const ServingAction & action) override;
    std::vector<std::size_t> idle() override;
    const RegionGraph & graph() const override;
    std::vector<std::size_t> retire() override;
    nlohmann::json summary() const;
    nlohmann::json document() const;
    PageReuse page_reuse() const;
    const ServingTopology & topology() const;
    std::vector<TransferBinding> transfer_bindings() const;

private:
    using RankPorts = std::map<std::size_t, std::vector<std::size_t>>;

    struct Handoff {
        RankPorts reserved;
        RankPorts prepared;
        RankPorts transferred;
        RankPorts received;
        std::set<std::size_t> accepted;
        std::set<std::size_t> released;
    };

    void require(std::span<const std::size_t> producers, std::size_t begin, const std::string & kind);
    void transfer(const ServingAction & action, Handoff & handoff);
    void bind_readiness(const ServingAction & action, const BatchProgram & program, std::span<const std::size_t> drained, RankPorts & ports);
    std::size_t append_transfer(const KvTransfer & route, std::int64_t bytes, std::int64_t segments, std::span<const std::size_t> prerequisites,
                                const std::string & kind, std::span<const KvSlice> slices);
    void count(std::size_t operation_begin, std::size_t node_begin, std::size_t dependency_begin);
    const std::string & resource_group(std::size_t rank) const;

    RegionGraph graph_;
    ServingTopology topology_;
    DisaggregatedGroups groups_;
    ServingStructure prefill_;
    ServingStructure decode_;
    KvTransfers transfers_;
    std::map<std::size_t, Handoff> handoffs_;
    std::size_t batches_ = 0;
    std::size_t actions_ = 0;
    std::size_t operations_ = 0;
    std::size_t nodes_ = 0;
    std::size_t dependencies_ = 0;
    std::int64_t kv_bytes_ = 0;
    std::int64_t metadata_bytes_ = 0;
};

} // namespace markov::sglang
