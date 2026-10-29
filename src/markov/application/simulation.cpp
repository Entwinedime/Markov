#include "markov/application/simulation.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "markov/adapters/frameworks/sglang/disaggregated.hpp"
#include "markov/adapters/frameworks/sglang/structure.hpp"
#include "markov/adapters/frameworks/sglang/topology.hpp"
#include "markov/controller/disaggregated.hpp"
#include "markov/controller/native.hpp"
#include "markov/controller/serving.hpp"
#include "markov/resources/kv_pool.hpp"
#include "markov/serving/workflow.hpp"

namespace markov {
namespace {

std::int64_t integer(const nlohmann::json & source) {
    if (!source.is_number_integer()) throw std::invalid_argument("serving fields must be integers");
    return source.get<std::int64_t>();
}

void check_fields(const nlohmann::json & source, std::initializer_list<std::string_view> fields, std::string_view context) {
    for (const auto & [name, _] : source.items()) {
        if (std::ranges::find(fields, std::string_view(name)) == fields.end())
            throw std::invalid_argument(std::string(context) + " has unsupported field: " + name);
    }
}

nlohmann::json expert_condition(const nlohmann::json & settings, const nlohmann::json & input, const TemplateAsset & source) {
    if (!settings.contains("ep")) return nullptr;
    const auto & ep = settings.at("ep");
    check_fields(ep, { "id", "size" }, "ep");
    const auto id = ep.at("id").get<std::string>();
    const auto size = integer(ep.at("size"));
    if (id.empty() || size <= 0 || size != source.environment().at("logical_ep").get<std::int64_t>())
        throw std::invalid_argument("EP topology does not match the native source");
    const auto & conditions = input.at("ep_routes");
    if (!conditions.is_object() || !conditions.contains(id)) throw std::invalid_argument("workload has no route condition for EP topology: " + id);
    return conditions.at(id);
}

nlohmann::json resource_capacity(const KvPool & resources) {
    const auto & capacity = resources.capacity();
    return {
        { "slots", capacity.slots },
        { "pages", capacity.pages }
    };
}

} // namespace

ServingSimulation::ServingSimulation(std::span<const TemplateAsset> assets, const nlohmann::json & input, const nlohmann::json & batches, ControlPeer * peer)
    : assets_(assets),
      input_(input),
      configuration_(input.at("configuration")),
      durations_(input.at("node_durations_ns")),
      peer_(peer) {
    check_fields(input_, { "configuration", "requests", "ep_routes", "node_durations_ns" }, "serving input");
    const bool disaggregated = configuration_.contains("pd");
    if (peer_ && !batches.is_null()) throw std::invalid_argument("native control does not consume batch replay");
    if (disaggregated) check_fields(configuration_, { "pd", "page_size", "placement" }, "configuration");
    else
        check_fields(configuration_,
                     { "max_running_requests", "max_total_tokens", "page_size", "max_prefill_tokens", "pp", "microbatch_requests", "placement", "ep" },
                     "configuration");
    if (input_.contains("ep_routes") && !configuration_.contains("ep") && !disaggregated)
        throw std::invalid_argument("workload routes require a configured EP topology");

    // Parse once before control and routing share the same request identity order.
    requests_ = Request::read(input_.at("requests"));
    if (!batches.is_null()) replay_.emplace(batches, requests_, disaggregated);

    if (configuration_.contains("placement")) {
        const auto & placement = configuration_.at("placement");
        check_fields(placement, { "devices" }, "placement");
        if (!placement.at("devices").is_array() || placement.at("devices").size() != assets_.size())
            throw std::invalid_argument("placement needs one device per logical rank");
        for (const auto & device : placement.at("devices")) devices_.push_back(integer(device));
    }
}

nlohmann::json ServingSimulation::run(bool details) {
    auto result = configuration_.contains("pd") ? disaggregated(details) : joint(details);
    if (replay_ && !replay_->done()) throw std::logic_error("serving completed with unconsumed batch records");
    result["control_source"] = peer_ ? "native_control" : replay_ ? "recorded_batches" : "lightweight_policy";
    result["configuration"] = configuration_;
    result["duration_source"] = "given_execution_kind_ns";
    return result;
}

nlohmann::json ServingSimulation::disaggregated(bool details) {
    const auto & pd = configuration_.at("pd");
    check_fields(pd, { "prefill", "decode" }, "pd");
    const auto & p = pd.at("prefill");
    const auto & d = pd.at("decode");
    check_fields(p, { "tp", "pp", "ep", "max_running_requests", "max_total_tokens", "max_prefill_tokens" }, "pd.prefill");
    check_fields(d, { "tp", "pp", "ep", "max_running_requests", "max_total_tokens" }, "pd.decode");
    if (input_.contains("ep_routes") && !p.contains("ep") && !d.contains("ep")) throw std::invalid_argument("workload routes require a configured EP topology");

    const TemplateAsset * prefill_source = nullptr;
    const TemplateAsset * decode_source = nullptr;
    for (const auto & asset : assets_) {
        const auto & mode = asset.environment().at("implementation").at("disaggregation_mode");
        if (mode == "prefill") prefill_source = &asset;
        if (mode == "decode") decode_source = &asset;
    }
    if (prefill_source == nullptr || decode_source == nullptr) throw std::invalid_argument("PD requires prefill and decode sources");
    for (const auto & [pool, source] : {
             std::pair{ &p, prefill_source },
             { &d,  decode_source }
    }) {
        const auto tp = integer(pool->at("tp"));
        const auto pp = pool->contains("pp") ? integer(pool->at("pp")) : 1;
        if (tp <= 0 || pp <= 0 || tp != source->environment().at("logical_tp") || pp != source->environment().at("logical_pp"))
            throw std::invalid_argument("PD target topology differs from its native instance source");
    }

    const auto page_size = integer(configuration_.at("page_size"));
    if (replay_
        && (replay_->lanes("prefill") > static_cast<std::size_t>(p.value("pp", 1)) || replay_->lanes("decode") > static_cast<std::size_t>(d.value("pp", 1))))
        throw std::invalid_argument("recorded batch lane exceeds the target pipeline");
    const ServingSettings prefill(integer(p.at("max_running_requests")), integer(p.at("max_total_tokens")), page_size, integer(p.at("max_prefill_tokens")));
    const ServingSettings decode(integer(d.at("max_running_requests")), integer(d.at("max_total_tokens")), page_size, 1);
    const DisaggregatedGroups groups{ "prefill", "decode", "kv" };
    sglang::DisaggregatedStructure expansion(assets_,
                                             prefill,
                                             decode,
                                             requests_,
                                             groups,
                                             devices_,
                                             expert_condition(p, input_, *prefill_source),
                                             expert_condition(d, input_, *decode_source));

    // Resource pools outlive both the controller and all borrowed stage participants.
    std::map<std::string, KvPool> resources;
    std::array instances{
        NativeInstance{ groups.prefill, NativeRole::Prefill, prefill, 0, {} },
        NativeInstance{  groups.decode,  NativeRole::Decode,  decode, 0, {} }
    };
    for (auto & instance : instances) {
        const auto stages = peer_ ? expansion.topology().stage_count(instance.id) : 1;
        instance.batch_limit = (static_cast<std::size_t>(instance.settings.max_running_requests) - 1) / stages + 1;
        for (std::size_t stage = 0; stage < stages; ++stage) {
            const auto & name = stages == 1 ? instance.id : expansion.topology().stage(instance.id, stage).id;
            auto [pool, _] = resources.try_emplace(name, requests_.size(), instance.settings, expansion.page_reuse());
            instance.participants.push_back({ name, stage, pool->second });
        }
    }

    std::unique_ptr<ServingControl> controller;
    if (peer_) {
        peer_->send({
            {      "kind",                                                                                   "source"                          },
            { "instances",
             { expansion.topology().control_source(*prefill_source, "prefill"), expansion.topology().control_source(*decode_source, "decode") } }
        });
        std::vector<NativeTransferRoute> transfers;
        for (const auto & binding : expansion.transfer_bindings())
            transfers.push_back({ binding, resources.at(binding.source), resources.at(binding.destination) });
        controller = std::make_unique<NativeController>(std::move(requests_), instances, *peer_, transfers);
    }
    else
        controller = std::make_unique<DisaggregatedController>(prefill,
                                                               decode,
                                                               std::move(requests_),
                                                               groups,
                                                               resources.at(groups.prefill),
                                                               resources.at(groups.decode),
                                                               replay_ ? &*replay_ : nullptr);

    ServingWorkflow workflow(*controller, expansion, durations_);
    auto result = workflow.run(details);
    for (const auto & [name, pool] : resources) {
        result["pool_free_pages"][name] = pool.free_pages().size();
        if (peer_) result["resource_capacity"][name] = resource_capacity(pool);
    }

    result["structure"] = expansion.summary();
    if (details) result["graph"] = expansion.document();
    return result;
}

nlohmann::json ServingSimulation::joint(bool details) {
    const ServingSettings settings{ integer(configuration_.at("max_running_requests")),
                                    integer(configuration_.at("max_total_tokens")),
                                    integer(configuration_.at("page_size")),
                                    integer(configuration_.at("max_prefill_tokens")) };
    const auto stages = configuration_.contains("pp") ? integer(configuration_.at("pp")) : 1;
    if (stages <= 0) throw std::invalid_argument("pp must be positive");
    if (replay_ && replay_->lanes("model") > static_cast<std::size_t>(stages)) throw std::invalid_argument("recorded batch lane exceeds the target pipeline");
    const auto batch_limit =
        configuration_.contains("microbatch_requests") ? integer(configuration_.at("microbatch_requests")) : (settings.max_running_requests - 1) / stages + 1;
    if (batch_limit <= 0 || batch_limit > settings.max_running_requests) throw std::invalid_argument("microbatch_requests must fit the request capacity");

    const sglang::ServingTopology topology(assets_, devices_);
    if (static_cast<std::size_t>(stages) != topology.stage_count("model")) throw std::invalid_argument("target PP differs from its source topology");
    const auto control_group = topology.parallel().groups().front().id;
    const auto routes = expert_condition(configuration_, input_, assets_.front());

    RegionGraph graph;
    sglang::ServingStructure expansion(graph, assets_, settings, requests_, topology, "model", routes);

    // Reserve before borrowing pool references into native participants.
    std::vector<KvPool> resources;
    resources.reserve(static_cast<std::size_t>(stages));
    for (std::int64_t stage = 0; stage < (peer_ ? stages : 1); ++stage) resources.emplace_back(requests_.size(), settings, expansion.page_reuse());

    std::unique_ptr<ServingControl> controller;
    if (peer_) {
        peer_->send({
            {      "kind",                                              "source" },
            { "instances", { topology.control_source(assets_.front(), "model") } }
        });
        std::vector<NativeParticipant> participants;
        for (std::size_t stage = 0; stage < resources.size(); ++stage) participants.push_back({ topology.stage("model", stage).id, stage, resources[stage] });
        const std::array instances{
            NativeInstance{ "model", NativeRole::Model, settings, static_cast<std::size_t>(batch_limit), std::move(participants) }
        };
        controller = std::make_unique<NativeController>(std::move(requests_), instances, *peer_);
    }
    else
        controller = std::make_unique<ServingController>(settings,
                                                         std::move(requests_),
                                                         resources.front(),
                                                         control_group,
                                                         static_cast<std::size_t>(stages),
                                                         static_cast<std::size_t>(batch_limit),
                                                         replay_ ? &*replay_ : nullptr);

    ServingWorkflow workflow(*controller, expansion, durations_);
    auto result = workflow.run(details);
    if (peer_) {
        for (std::size_t stage = 0; stage < resources.size(); ++stage)
            result["resource_capacity"][topology.stage("model", stage).id] = resource_capacity(resources[stage]);
    }

    result["structure"] = expansion.summary();
    if (details) result["graph"] = expansion.document();
    return result;
}

} // namespace markov
