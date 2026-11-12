#include "markov/cli/serving.hpp"

#include <algorithm>
#include <iostream>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

#include "markov/adapters/sglang/actions.hpp"
#include "markov/controller/native.hpp"
#include "markov/serving/session.hpp"
#include "markov/controller/execution_inputs.hpp"

namespace markov {
namespace {

class StreamPeer final : public ControlPeer {
public:
    nlohmann::json receive() override {
        std::string line;
        if (!std::getline(std::cin, line)) throw std::runtime_error("native control endpoint closed");

        // Keep unexpected native output diagnosable without accepting it as a control message.
        try {
            return nlohmann::json::parse(line);
        }
        catch (const nlohmann::json::parse_error & error) {
            throw std::runtime_error("native control sent an invalid JSON frame: " + line.substr(0, 160) + " (" + error.what() + ")");
        }
    }
    void send(const nlohmann::json & message) override {
        std::cout << message << '\n' << std::flush;
        if (!std::cout) throw std::runtime_error("native control endpoint cannot receive");
    }
};

struct Participant {
    Participant(const TemplateLibrary & library, const nlohmann::json & environment, std::vector<Resource> execution_resources, const std::string & group,
                const std::string & instance, const ServingSettings & capacity, std::span<const Request> requests, ResourceObserver * observer)
        : pool(requests.size(), capacity),
          resources(pool),
          graphs(library, environment, std::move(execution_resources), group, instance),
          dependencies(pool, resources, requests, observer) {}

    KvPool pool;
    ResourceService resources;
    sglang::NativeActionGraphs graphs;
    GraphDependencies dependencies;
};

} // namespace

nlohmann::json serve(const std::map<std::size_t, TemplateLibrary> & libraries, const nlohmann::json & task, ExecutionObserver * execution_observer,
                     ResourceObserver * resource_observer, ControlObserver * control_observer) {
    const auto & workload = task.at("workload");
    auto requests = Request::read(workload.at("requests"));
    const auto duration = read_parameters({
                                              { "given_work_ns", task.at("given_work_ns") }
    })
                              .at("given_work_ns");
    if (duration < 0) throw std::invalid_argument("given work duration must be nonnegative nanoseconds");
    const auto timing = task.value("execution_inputs", nlohmann::json());
    if (!timing.is_null() && !timing.is_object()) throw std::invalid_argument("execution inputs require an object");
    std::optional<ExecutionInputs> inputs;
    if (!timing.is_null()) inputs.emplace(timing, control_observer);
    const auto ingress = timing.is_null() ? nlohmann::json() : timing.at("ingress");
    if (!ingress.is_null() && !ingress.is_object()) throw std::invalid_argument("ingress must be an instance mapping");
    std::vector<Resource> resources;
    std::map<std::string, std::unique_ptr<Participant>> participants;
    std::map<std::string, ServingParticipant> execution;
    std::vector<NativeInstance> instances;
    std::set<std::string> instance_ids;
    auto sources = nlohmann::json::array();
    for (const auto & description : task.at("instances").get_ref<const nlohmann::json::array_t &>()) {
        const auto id = description.at("id").get<std::string>();
        if (id.empty() || !instance_ids.insert(id).second) throw std::invalid_argument("serving instance identities must be unique and nonempty");
        const auto settings = read_parameters(description.at("settings"));
        const ServingSettings capacity(settings.at("max_running_requests"),
                                       settings.at("max_total_tokens"),
                                       settings.at("page_size"),
                                       settings.at("max_prefill_tokens"));
        const auto & policy = description.at("control");
        std::optional<std::int64_t> microbatch_requests;
        if (const auto found = settings.find("microbatch_requests"); found != settings.end()) {
            if (found->second <= 0) throw std::invalid_argument("microbatch_requests must be positive when specified");
            microbatch_requests = found->second;
        }

        // Unspecified microbatch capacity is computed by the native scheduler for its actual pipeline depth.
        instances.push_back({ id,
                              capacity,
                              microbatch_requests,
                              {},
                              settings.at("prefill_chunk_tokens"),
                              policy.at("mixed_batches").get<bool>(),
                              description.at("expert_routes") });
        auto & instance = instances.back();
        nlohmann::json implementation;
        nlohmann::json runtime;
        std::vector<std::size_t> ranks;
        auto workers = nlohmann::json::array();
        std::int64_t tp = 0, pp = 0, ep = 0;
        for (const auto & binding : description.at("participants").get_ref<const nlohmann::json::array_t &>()) {
            const auto group = binding.at("id").get<std::string>();
            if (group.empty() || participants.contains(group)) throw std::invalid_argument("serving participant identities must be unique and nonempty");
            const auto & library = libraries.at(binding.at("template").get<std::size_t>());
            const auto & environment = binding.at("environment");
            const auto & parallel = environment.at("parallel");
            if (ranks.empty()) {
                implementation = environment.at("implementation");
                runtime = { { "framework_revision", implementation.at("framework_revision") }, { "torch", environment.at("torch") } };
                tp = parallel.at("logical_tp").get<std::int64_t>();
                pp = parallel.at("logical_pp").get<std::int64_t>();
                ep = parallel.at("logical_ep").get<std::int64_t>();
                if (tp <= 0 || pp <= 0 || ep <= 0 || ep > tp || tp % ep != 0) throw std::invalid_argument("invalid native parallel topology");
            }
            if (environment.at("implementation") != implementation || parallel.at("logical_tp") != tp || parallel.at("logical_pp") != pp
                || parallel.at("logical_ep") != ep || implementation.at("page_size") != capacity.page_size || environment.at("torch") != runtime.at("torch"))
                throw std::invalid_argument("native participants disagree on their instance implementation");
            const auto local = static_cast<std::int64_t>(ranks.size());
            if (parallel.at("tp_rank") != local % tp || parallel.at("pp_rank") != local / tp)
                throw std::invalid_argument("participants must follow their native TP/PP rank order");
            const auto rank = read_parameters({
                                                  { "rank", binding.at("rank") }
            })
                                  .at("rank");
            if (rank < 0 || parallel.at("rank") != rank) throw std::invalid_argument("participant rank must match its nonnegative environment rank");
            ranks.push_back(static_cast<std::size_t>(rank));
            workers.push_back({
                { "rank", rank },
                { "group", group },
                { "ep_rank", parallel.at("ep_rank") },
                { "expert_layers", environment.value("expert_layers", nlohmann::json::array()) },
                { "kv_layout", environment.value("kv_layout", nlohmann::json()) },
                { "kv_peers", environment.value("kv_peers", nlohmann::json::object()) }
            });
            auto local_resources = library.resources;
            for (std::size_t index = 0; index < local_resources.size(); ++index) {
                local_resources[index].identity = group + "/resource/" + std::to_string(index);
                local_resources[index].rank = rank;
            }
            resources.insert(resources.end(), local_resources.begin(), local_resources.end());
            const auto control_count = std::ranges::count_if(local_resources, [](const auto & resource) {
                return resource.kind == ResourceKind::Thread && resource.role == "control";
            });
            if (control_count != 1) throw std::invalid_argument("native CPU services require one declared control thread per participant");
            const auto control_thread = *std::ranges::find_if(local_resources, [](const auto & resource) {
                return resource.kind == ResourceKind::Thread && resource.role == "control";
            });
            auto & participant =
                *participants
                     .emplace(group,
                              std::make_unique<Participant>(library, environment, std::move(local_resources), group, id, capacity, requests, resource_observer))
                     .first->second;
            instance.participants.push_back({ group, static_cast<std::size_t>(rank), control_thread,
                                               participant.resources, participant.graphs, participant.dependencies });
            execution.emplace(group, ServingParticipant{ participant.graphs, participant.dependencies });
        }
        if ((tp == 0 || ranks.size() % static_cast<std::size_t>(tp) != 0 || ranks.size() / static_cast<std::size_t>(tp) != static_cast<std::size_t>(pp))
            || std::set(ranks.begin(), ranks.end()).size() != ranks.size() || ranks.empty())
            throw std::invalid_argument("native instance requires exactly one participant for each rank");

        // Requested control policy must use the implementation actually observed in the source.
        if (implementation.at("cache").at("disabled") != policy.at("disable_radix_cache") || implementation.at("disaggregation_mode") != description.at("mode"))
            throw std::invalid_argument("native instance control differs from its observed cache or disaggregation mode");
        const nlohmann::json scheduling{
            { "chunked_prefill", settings.at("prefill_chunk_tokens") > 0 },
            {   "mixed_batches",              policy.at("mixed_batches") }
        };
        if (implementation.at("scheduling") != scheduling)
            throw std::invalid_argument("native instance scheduling mechanisms differ from the observed implementation");
        sources.push_back({
            { "id", id },
            { "runtime", std::move(runtime) },
            { "model_config", implementation.at("model") },
            { "allocator", implementation.at("allocator") },
            { "expert_layout", implementation.value("expert_layout", nlohmann::json()) },
            { "dtype", implementation.at("dtype") },
            { "tp", tp },
            { "pp", pp },
            { "ep", ep },
            { "workers", std::move(workers) },
            { "mode", description.at("mode") },
            { "moe_a2a_backend", policy.at("moe_a2a_backend") },
            { "deepep_mode", policy.at("deepep_mode") },
            { "cache", implementation.at("cache") }
        });
    }
    if (instances.empty()) throw std::invalid_argument("serving requires a native instance");
    StreamPeer peer;
    peer.send({
        {      "kind",           "source" },
        { "instances", std::move(sources) }
    });
    Timeline timeline(std::move(resources), execution_observer);
    NativeController control(std::move(requests), instances, peer, workload.at("prefixes"), timeline, ingress, duration,
                             inputs ? &*inputs : nullptr, resource_observer, control_observer);
    auto result = ServingSession(control, timeline, std::move(execution)).run(duration, inputs ? &*inputs : nullptr);
    result.update(control.cpu_summary());
    if (!ingress.is_null()) result["arrival_basis"] = "supplied_ingress";
    return result;
}

} // namespace markov
