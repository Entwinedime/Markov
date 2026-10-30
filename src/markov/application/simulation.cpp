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
#include "markov/resources/trace.hpp"
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

nlohmann::json resource_state(const KvPool & resources, const ServingControl & control, bool details) {
    const auto & capacity = resources.capacity();
    const auto & prefixes = resources.prefixes();
    const auto resident = [](const KvResidence & request) { return request.slot || request.allocated_tokens != 0 || !request.pages.empty(); };
    nlohmann::json result{
        { "capacity", { { "slots", capacity.slots }, { "pages", capacity.pages } } },
        { "free_slots", resources.free_slots().size() },
        { "free_pages", resources.free_pages().size() },
        { "deferred_free_pages", resources.deferred_pages() },
        { "evictable_pages", prefixes.evictable_tokens() / resources.page_size() },
        { "protected_pages", prefixes.protected_tokens() / resources.page_size() },
        { "resident_requests", std::ranges::count_if(resources.residences(), resident) }
    };
    if (!details) return result;

    // Export owner facts only on demand; evaluation does not add another resource ledger to serving.
    auto & ownership = result["ownership"];
    ownership["free_slots"] = resources.free_slots();
    ownership["free_pages"] = resources.free_pages();
    ownership["requests"] = nlohmann::json::array();
    for (std::size_t identity = 0; identity < resources.residences().size(); ++identity) {
        const auto & request = resources.residence(identity);
        if (resident(request))
            ownership["requests"].push_back({
                {               "id",                                           control.request(identity).id },
                {             "slot", request.slot ? nlohmann::json(*request.slot) : nlohmann::json(nullptr) },
                { "allocated_tokens",                                               request.allocated_tokens },
                {            "pages",                                                          request.pages }
            });
    }
    ownership["prefixes"] = nlohmann::json::array();
    for (const auto & binding : prefixes.residences())
        ownership["prefixes"].push_back({
            {       "node",       binding.node },
            { "references", binding.references },
            {      "pages",      binding.pages }
        });
    return result;
}

} // namespace

ServingSimulation::ServingSimulation(std::span<const TemplateAsset> assets, const nlohmann::json & input, const nlohmann::json & batches, ControlPeer * peer)
    : assets_(assets),
      input_(input),
      configuration_(input.at("configuration")),
      durations_(input.at("node_durations_ns")),
      peer_(peer) {
    check_fields(input_, { "configuration", "requests", "prefixes", "ep_routes", "node_durations_ns" }, "serving input");
    const bool disaggregated = configuration_.contains("pd");
    if (peer_ && !batches.is_null()) throw std::invalid_argument("native control does not consume batch replay");
    if (disaggregated) check_fields(configuration_, { "pd", "page_size", "placement" }, "configuration");
    else
        check_fields(configuration_,
                     { "max_running_requests",
                       "max_total_tokens",
                       "page_size",
                       "max_prefill_tokens",
                       "prefill_chunk_tokens",
                       "mixed_batches",
                       "prefix_cache",
                       "pp",
                       "microbatch_requests",
                       "placement",
                       "ep" },
                     "configuration");
    if (input_.contains("ep_routes") && !configuration_.contains("ep") && !disaggregated)
        throw std::invalid_argument("workload routes require a configured EP topology");

    // Source and target must describe the same cache mechanism at every participant.
    for (const auto & asset : assets_) {
        const auto & implementation = asset.environment().at("implementation");
        const auto & pool = disaggregated ? configuration_.at("pd").at(implementation.at("disaggregation_mode").get<std::string>()) : configuration_;
        const auto enabled = pool.value("prefix_cache", false);
        if (enabled == implementation.value("disable_radix_cache", true)) throw std::invalid_argument("target prefix cache differs from its source mechanism");
        if (enabled && !peer_) throw std::invalid_argument("prefix cache requires native control");
    }

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
    std::optional<ResourceTrace> recording;
    if (details) recording.emplace();
    auto * trace = recording ? &*recording : nullptr;
    auto result = configuration_.contains("pd") ? disaggregated(trace) : joint(trace);
    if (trace) result["resource_history"] = trace->take();
    if (replay_ && !replay_->done()) throw std::logic_error("serving completed with unconsumed batch records");
    result["control_source"] = peer_ ? "native_control" : replay_ ? "recorded_batches" : "lightweight_policy";
    result["configuration"] = configuration_;
    result["duration_source"] = "given_execution_kind_ns";
    return result;
}

std::int64_t ServingSimulation::prefill_chunk_tokens(const nlohmann::json & pool, const ServingSettings & settings) const {
    if (!pool.contains("prefill_chunk_tokens")) return 0;
    const auto tokens = integer(pool.at("prefill_chunk_tokens"));
    if (tokens <= 0 || tokens % settings.page_size != 0) throw std::invalid_argument("prefill_chunk_tokens must be positive and page aligned");
    if (!peer_) throw std::invalid_argument("chunked Prefill requires native control");
    for (const auto & asset : assets_) {
        const auto & implementation = asset.environment().at("implementation");
        if (configuration_.contains("pd") && implementation.at("disaggregation_mode") == "decode") continue;
        if (implementation.at("chunked_prefill_size").get<std::int64_t>() <= 0)
            throw std::invalid_argument("chunked Prefill requires a source captured with that mechanism");
    }
    return tokens;
}

nlohmann::json ServingSimulation::disaggregated(ResourceTrace * trace) {
    const bool details = trace != nullptr;
    const auto & pd = configuration_.at("pd");
    check_fields(pd, { "prefill", "decode" }, "pd");
    const auto & p = pd.at("prefill");
    const auto & d = pd.at("decode");
    check_fields(p,
                 { "tp", "pp", "ep", "max_running_requests", "max_total_tokens", "max_prefill_tokens", "prefill_chunk_tokens", "prefix_cache" },
                 "pd.prefill");
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
        NativeInstance{ groups.prefill, NativeRole::Prefill, prefill, 0, {}, prefill_chunk_tokens(p, prefill) },
        NativeInstance{ groups.decode, NativeRole::Decode, decode, 0, {} }
    };
    instances[0].expert_routes = expert_condition(p, input_, *prefill_source);
    instances[1].expert_routes = expert_condition(d, input_, *decode_source);
    for (auto & instance : instances) {
        const auto stages = peer_ ? expansion.topology().stage_count(instance.id) : 1;
        instance.batch_limit = (static_cast<std::size_t>(instance.settings.max_running_requests) - 1) / stages + 1;
        for (std::size_t stage = 0; stage < stages; ++stage) {
            const auto & name = stages == 1 ? instance.id : expansion.topology().stage(instance.id, stage).id;
            auto [pool, _] = resources.try_emplace(name, requests_.size(), instance.settings, expansion.page_reuse(), trace, name);
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
        controller = std::make_unique<NativeController>(std::move(requests_), instances, *peer_, input_.value("prefixes", nlohmann::json::array()), transfers);
    }
    else
        controller = std::make_unique<DisaggregatedController>(prefill,
                                                               decode,
                                                               std::move(requests_),
                                                               groups,
                                                               resources.at(groups.prefill),
                                                               resources.at(groups.decode),
                                                               replay_ ? &*replay_ : nullptr);

    ServingWorkflow workflow(*controller, expansion, durations_, trace);
    auto result = workflow.run(details);
    for (const auto & [name, pool] : resources) result["resources"][name] = resource_state(pool, *controller, details);

    result["structure"] = expansion.summary();
    if (details) result["graph"] = expansion.document();
    return result;
}

nlohmann::json ServingSimulation::joint(ResourceTrace * trace) {
    const bool details = trace != nullptr;
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
    const auto chunk_tokens = prefill_chunk_tokens(configuration_, settings);
    const auto mixed_batches = configuration_.value("mixed_batches", false);
    if (mixed_batches) {
        if (chunk_tokens == 0) throw std::invalid_argument("mixed batches require native chunked Prefill");
        for (const auto & asset : assets_) {
            if (!asset.environment().at("implementation").at("enable_mixed_chunk").get<bool>())
                throw std::invalid_argument("mixed batches require a source with that mechanism enabled");
        }
    }

    const sglang::ServingTopology topology(assets_, devices_);
    if (static_cast<std::size_t>(stages) != topology.stage_count("model")) throw std::invalid_argument("target PP differs from its source topology");
    const auto control_group = topology.parallel().groups().front().id;
    const auto routes = expert_condition(configuration_, input_, assets_.front());

    RegionGraph graph;
    sglang::ServingStructure expansion(graph, assets_, settings, requests_, topology, "model", routes);

    // Reserve before borrowing pool references into native participants.
    std::vector<KvPool> resources;
    resources.reserve(static_cast<std::size_t>(stages));
    for (std::int64_t stage = 0; stage < (peer_ ? stages : 1); ++stage) {
        const auto & name = peer_ ? topology.stage("model", static_cast<std::size_t>(stage)).id : control_group;
        resources.emplace_back(requests_.size(), settings, expansion.page_reuse(), trace, name);
    }

    std::unique_ptr<ServingControl> controller;
    if (peer_) {
        peer_->send({
            {      "kind",                                              "source" },
            { "instances", { topology.control_source(assets_.front(), "model") } }
        });
        std::vector<NativeParticipant> participants;
        for (std::size_t stage = 0; stage < resources.size(); ++stage) participants.push_back({ topology.stage("model", stage).id, stage, resources[stage] });
        const std::array instances{
            NativeInstance{ "model",
                           NativeRole::Model,
                           settings, static_cast<std::size_t>(batch_limit),
                           std::move(participants),
                           chunk_tokens, mixed_batches,
                           routes }
        };
        controller = std::make_unique<NativeController>(std::move(requests_), instances, *peer_, input_.value("prefixes", nlohmann::json::array()));
    }
    else
        controller = std::make_unique<ServingController>(settings,
                                                         std::move(requests_),
                                                         resources.front(),
                                                         control_group,
                                                         static_cast<std::size_t>(stages),
                                                         static_cast<std::size_t>(batch_limit),
                                                         replay_ ? &*replay_ : nullptr);

    ServingWorkflow workflow(*controller, expansion, durations_, trace);
    auto result = workflow.run(details);
    for (std::size_t stage = 0; stage < resources.size(); ++stage) {
        const auto & name = peer_ ? topology.stage("model", stage).id : control_group;
        result["resources"][name] = resource_state(resources[stage], *controller, details);
    }

    result["structure"] = expansion.summary();
    if (details) result["graph"] = expansion.document();
    return result;
}

} // namespace markov
