#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "markov/adapters/frameworks/sglang/batch_record.hpp"
#include "markov/adapters/frameworks/sglang/disaggregated.hpp"
#include "markov/adapters/frameworks/sglang/structure.hpp"
#include "markov/adapters/frameworks/sglang/topology.hpp"
#include "markov/controller/disaggregated.hpp"
#include "markov/controller/native.hpp"
#include "markov/controller/serving.hpp"
#include "markov/execution/durations.hpp"
#include "markov/graph/source_graph.hpp"
#include "markov/production/source_projection.hpp"
#include "markov/resources/kv_pool.hpp"
#include "markov/templates/instance.hpp"
#include "markov/templates/library.hpp"
#include "markov/templates/target_graph.hpp"
#include "markov/workflow/control_stream.hpp"
#include "markov/workflow/serving.hpp"

namespace {

nlohmann::json read_json(const std::filesystem::path & path) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("cannot open " + path.string());

    nlohmann::json result;
    input >> result;
    return result;
}

void write_json(const std::filesystem::path & path, const nlohmann::json & result, const std::vector<std::filesystem::path> & inputs, bool readable = false) {
    const std::filesystem::path temporary = path.string() + ".tmp";
    const auto destination = std::filesystem::weakly_canonical(path);
    const auto staging = std::filesystem::weakly_canonical(temporary);

    // Publication and staging must both preserve declared inputs, including a hard-linked staging file.
    for (const auto & input : inputs) {
        const auto source = std::filesystem::weakly_canonical(input);
        if (destination == source || staging == source || (std::filesystem::exists(temporary) && std::filesystem::equivalent(temporary, input)))
            throw std::invalid_argument("output or staging path must not overwrite an input");
    }

    // Publish only after serialization succeeds, keeping the previous artifact intact on failure.
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
    std::ofstream output(temporary);
    output.exceptions(std::ios::badbit | std::ios::failbit);
    if (readable) output << std::setw(2);
    output << result << '\n';
    output.close();
    std::filesystem::rename(temporary, path);
}

nlohmann::json compose(const markov::TemplateAsset & asset, const nlohmann::json & target) {
    markov::RegionGraph shared;
    shared.execution_resources = asset.execution_resources();
    markov::TargetGraph graph(asset, shared);
    std::vector<markov::TargetRegion> calls;
    const auto & invocations = target.at("invocations");
    if (!invocations.is_array() || invocations.empty()) throw std::invalid_argument("composition requires a nonempty invocation array");

    for (const auto & call : invocations) {
        std::map<std::string, std::size_t> inputs;
        if (call.contains("inputs")) {
            for (const auto & [name, source] : call.at("inputs").items()) {
                const auto & previous = calls.at(source.at("call").get<std::size_t>());
                inputs.emplace(name, previous.ports.outputs.at(source.at("port").get<std::string>()));
            }
        }

        calls.push_back(graph.append(call.at("role").get<std::string>(), call.at("phase").get<std::string>(), markov::Invocation::read(call), inputs));
    }

    return graph.document();
}

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

nlohmann::json expert_condition(const nlohmann::json & settings, const nlohmann::json & input, const markov::TemplateAsset & source) {
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

nlohmann::json resource_capacity(const markov::KvPool & resources) {
    const auto & capacity = resources.capacity();
    return {
        { "slots", capacity.slots },
        { "pages", capacity.pages }
    };
}

nlohmann::json native_source(const markov::TemplateAsset & asset, const std::string & id, const markov::sglang::ServingTopology & topology) {
    const auto & environment = asset.environment();
    const auto & implementation = environment.at("implementation");
    return {
        {           "id",                                       id },
        {        "ranks",            topology.instance(id).members },
        { "model_config",        implementation.at("model_config") },
        {        "dtype",               implementation.at("dtype") },
        {         "mode", implementation.at("disaggregation_mode") },
        {           "tp",             environment.at("logical_tp") },
        {           "ep",             environment.at("logical_ep") },
        {           "pp",             environment.at("logical_pp") }
    };
}

nlohmann::json simulate(std::span<const markov::TemplateAsset> assets, const nlohmann::json & input, const nlohmann::json & batches, bool details,
                        bool native) {
    check_fields(input, { "configuration", "requests", "ep_routes", "node_durations_ns" }, "serving input");
    const auto & configuration = input.at("configuration");
    const bool disaggregated = configuration.contains("pd");
    if (native && !batches.is_null()) throw std::invalid_argument("native control does not consume batch replay");
    if (disaggregated) check_fields(configuration, { "pd", "page_size", "placement" }, "configuration");
    else
        check_fields(configuration,
                     { "max_running_requests", "max_total_tokens", "page_size", "max_prefill_tokens", "pp", "microbatch_requests", "placement", "ep" },
                     "configuration");
    if (input.contains("ep_routes") && !configuration.contains("ep") && !disaggregated)
        throw std::invalid_argument("workload routes require a configured EP topology");

    std::vector<markov::Request> requests;
    if (!input.at("requests").is_array() || input.at("requests").empty()) throw std::invalid_argument("serving needs a nonempty request array");
    for (const auto & request : input.at("requests")) {
        check_fields(request, { "id", "arrival_ns", "prompt_tokens", "output_tokens" }, "request");
        requests.push_back({ request.at("id").get<std::string>(),
                             integer(request.at("arrival_ns")),
                             integer(request.at("prompt_tokens")),
                             integer(request.at("output_tokens")) });
    }

    requests = markov::Request::ordered(std::move(requests));
    std::optional<markov::BatchReplay> replay;
    if (!batches.is_null()) replay.emplace(batches, requests, disaggregated);

    const markov::GivenDurations durations(input.at("node_durations_ns"));
    std::vector<std::int64_t> devices;
    if (configuration.contains("placement")) {
        const auto & placement = configuration.at("placement");
        check_fields(placement, { "devices" }, "placement");
        if (!placement.at("devices").is_array() || placement.at("devices").size() != assets.size())
            throw std::invalid_argument("placement needs one device per logical rank");
        for (const auto & device : placement.at("devices")) devices.push_back(integer(device));
    }
    if (disaggregated) {
        const auto & pd = configuration.at("pd");
        check_fields(pd, { "prefill", "decode" }, "pd");
        const auto & p = pd.at("prefill");
        const auto & d = pd.at("decode");
        check_fields(p, { "tp", "pp", "ep", "max_running_requests", "max_total_tokens", "max_prefill_tokens" }, "pd.prefill");
        check_fields(d, { "tp", "pp", "ep", "max_running_requests", "max_total_tokens" }, "pd.decode");
        if (input.contains("ep_routes") && !p.contains("ep") && !d.contains("ep"))
            throw std::invalid_argument("workload routes require a configured EP topology");
        const markov::TemplateAsset * prefill_source = nullptr;
        const markov::TemplateAsset * decode_source = nullptr;
        for (const auto & asset : assets) {
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
        const auto page_size = integer(configuration.at("page_size"));
        if (replay
            && (replay->lanes("prefill") > static_cast<std::size_t>(p.value("pp", 1)) || replay->lanes("decode") > static_cast<std::size_t>(d.value("pp", 1))))
            throw std::invalid_argument("recorded batch lane exceeds the target pipeline");
        const markov::ServingSettings prefill(integer(p.at("max_running_requests")),
                                              integer(p.at("max_total_tokens")),
                                              page_size,
                                              integer(p.at("max_prefill_tokens")));
        const markov::ServingSettings decode(integer(d.at("max_running_requests")), integer(d.at("max_total_tokens")), page_size, 1);
        const markov::DisaggregatedGroups groups{ "prefill", "decode", "kv" };
        markov::sglang::DisaggregatedStructure expansion(assets,
                                                         prefill,
                                                         decode,
                                                         requests,
                                                         groups,
                                                         devices,
                                                         expert_condition(p, input, *prefill_source),
                                                         expert_condition(d, input, *decode_source));
        std::map<std::string, markov::KvPool> resources;
        std::array instances{
            markov::NativeInstance{ groups.prefill, markov::NativeRole::Prefill, prefill, 0, {} },
            markov::NativeInstance{  groups.decode,  markov::NativeRole::Decode,  decode, 0, {} }
        };
        for (auto & instance : instances) {
            const auto stages = native ? expansion.topology().stage_count(instance.id) : 1;
            instance.batch_limit = (static_cast<std::size_t>(instance.settings.max_running_requests) - 1) / stages + 1;
            for (std::size_t stage = 0; stage < stages; ++stage) {
                const auto & name = stages == 1 ? instance.id : expansion.topology().stage(instance.id, stage).id;
                auto [pool, _] = resources.try_emplace(name, requests.size(), instance.settings, expansion.page_reuse());
                instance.participants.push_back({ name, stage, pool->second });
            }
        }
        markov::ControlStream peer(std::cin, std::cout);
        std::unique_ptr<markov::ServingControl> controller;
        if (native) {
            peer.send({
                {      "kind",                                                                               "source"                              },
                { "instances",
                 { native_source(*prefill_source, "prefill", expansion.topology()), native_source(*decode_source, "decode", expansion.topology()) } }
            });
            std::vector<markov::NativeTransferRoute> transfers;
            for (const auto & binding : expansion.transfer_bindings())
                transfers.push_back({ binding, resources.at(binding.source), resources.at(binding.destination) });
            controller = std::make_unique<markov::NativeController>(std::move(requests), instances, peer, transfers);
        }
        else
            controller = std::make_unique<markov::DisaggregatedController>(prefill,
                                                                           decode,
                                                                           std::move(requests),
                                                                           groups,
                                                                           resources.at(groups.prefill),
                                                                           resources.at(groups.decode),
                                                                           replay ? &*replay : nullptr);
        markov::ServingWorkflow workflow(*controller, expansion, durations);
        auto result = workflow.run(details);
        if (replay && !replay->done()) throw std::logic_error("serving completed with unconsumed batch records");
        result["control_source"] = native ? "native_control" : replay ? "recorded_batches" : "lightweight_policy";
        result["configuration"] = configuration;
        result["duration_source"] = "given_execution_kind_ns";
        for (const auto & [name, pool] : resources) {
            result["pool_free_pages"][name] = pool.free_pages().size();
            if (native) result["resource_capacity"][name] = resource_capacity(pool);
        }
        result["structure"] = expansion.summary();
        if (details) result["graph"] = expansion.document();
        return result;
    }

    const markov::ServingSettings settings{ integer(configuration.at("max_running_requests")),
                                            integer(configuration.at("max_total_tokens")),
                                            integer(configuration.at("page_size")),
                                            integer(configuration.at("max_prefill_tokens")) };
    const auto stages = configuration.contains("pp") ? integer(configuration.at("pp")) : 1;
    if (stages <= 0) throw std::invalid_argument("pp must be positive");
    if (replay && replay->lanes("model") > static_cast<std::size_t>(stages)) throw std::invalid_argument("recorded batch lane exceeds the target pipeline");
    const auto batch_limit =
        configuration.contains("microbatch_requests") ? integer(configuration.at("microbatch_requests")) : (settings.max_running_requests - 1) / stages + 1;
    if (batch_limit <= 0 || batch_limit > settings.max_running_requests) throw std::invalid_argument("microbatch_requests must fit the request capacity");
    const markov::sglang::ServingTopology topology(assets, devices);
    if (static_cast<std::size_t>(stages) != topology.stage_count("model")) throw std::invalid_argument("target PP differs from its source topology");
    const auto control_group = topology.parallel().groups().front().id;
    const auto routes = expert_condition(configuration, input, assets.front());
    markov::RegionGraph graph;
    markov::sglang::ServingStructure expansion(graph, assets, settings, requests, topology, "model", routes);
    std::vector<markov::KvPool> resources;
    resources.reserve(static_cast<std::size_t>(stages));
    for (std::int64_t stage = 0; stage < (native ? stages : 1); ++stage) resources.emplace_back(requests.size(), settings, expansion.page_reuse());
    markov::ControlStream peer(std::cin, std::cout);
    std::unique_ptr<markov::ServingControl> controller;
    if (native) {
        peer.send({
            {      "kind",                                             "source" },
            { "instances", { native_source(assets.front(), "model", topology) } }
        });
        std::vector<markov::NativeParticipant> participants;
        for (std::size_t stage = 0; stage < resources.size(); ++stage) participants.push_back({ topology.stage("model", stage).id, stage, resources[stage] });
        const std::array instances{
            markov::NativeInstance{ "model", markov::NativeRole::Model, settings, static_cast<std::size_t>(batch_limit), std::move(participants) }
        };
        controller = std::make_unique<markov::NativeController>(std::move(requests), instances, peer);
    }
    else
        controller = std::make_unique<markov::ServingController>(settings,
                                                                 std::move(requests),
                                                                 resources.front(),
                                                                 control_group,
                                                                 static_cast<std::size_t>(stages),
                                                                 static_cast<std::size_t>(batch_limit),
                                                                 replay ? &*replay : nullptr);
    markov::ServingWorkflow workflow(*controller, expansion, durations);
    auto result = workflow.run(details);
    if (replay && !replay->done()) throw std::logic_error("serving completed with unconsumed batch records");
    result["control_source"] = native ? "native_control" : replay ? "recorded_batches" : "lightweight_policy";
    result["configuration"] = configuration;
    result["duration_source"] = "given_execution_kind_ns";
    if (native) {
        for (std::size_t stage = 0; stage < resources.size(); ++stage)
            result["resource_capacity"][topology.stage("model", stage).id] = resource_capacity(resources[stage]);
    }
    result["structure"] = expansion.summary();
    if (details) result["graph"] = expansion.document();
    return result;
}

const markov::TemplateAsset & select_role(std::span<const markov::TemplateAsset> roles, const nlohmann::json & target) {
    const auto rank = target.contains("source_rank") ? integer(target.at("source_rank")) : 0;
    const auto selected = std::ranges::find_if(roles, [rank](const auto & role) { return role.environment().at("rank") == rank; });
    if (selected == roles.end()) throw std::invalid_argument("template catalog has no selected source rank");
    return *selected;
}

} // namespace

int main(int argc, char ** argv) {
    try {
        if (argc >= 2 && std::string(argv[1]) == "record-batches") {
            std::vector<std::filesystem::path> sources;
            std::filesystem::path input;
            std::filesystem::path output;
            for (int index = 2; index < argc; ++index) {
                const std::string option = argv[index];
                if (index + 1 == argc) throw std::invalid_argument("record-batches option has no value");
                const std::filesystem::path value = argv[++index];
                if (option == "--source") sources.push_back(value);
                else if (option == "--input" && input.empty()) input = value;
                else if (option == "--output" && output.empty()) output = value;
                else throw std::invalid_argument("unknown or repeated record-batches option: " + option);
            }
            if (sources.empty() || input.empty() || output.empty()) throw std::invalid_argument("record-batches requires source, input and output");
            const auto specification = read_json(input);
            markov::sglang::BatchRecord record(specification.at("requests"));
            std::vector<std::filesystem::path> inputs{ input };
            for (const auto & source : sources) {
                const auto session = source.parent_path() / "session.json";
                record.append(read_json(source), read_json(session));
                inputs.insert(inputs.end(), { source, session });
            }
            std::vector<markov::Request> requests;
            for (const auto & request : specification.at("requests"))
                requests.push_back({ request.at("id").get<std::string>(),
                                     integer(request.at("arrival_ns")),
                                     integer(request.at("prompt_tokens")),
                                     integer(request.at("output_tokens")) });
            const markov::BatchReplay checked(record.document(),
                                              markov::Request::ordered(std::move(requests)),
                                              specification.at("configuration").contains("pd"));
            write_json(output, record.document(), inputs, true);
            std::cout << "native batch records written\n";
            return 0;
        }
        if (argc >= 2 && std::string(argv[1]) == "simulate") {
            std::filesystem::path template_path;
            std::filesystem::path input;
            std::filesystem::path output;
            std::filesystem::path batches;
            bool details = false;
            std::string control;
            for (int index = 2; index < argc; ++index) {
                const std::string option = argv[index];
                if (option == "--details" && !details) {
                    details = true;
                    continue;
                }
                if (index + 1 == argc) throw std::invalid_argument("simulate option has no value");
                const std::string value = argv[++index];
                if (option == "--templates" && template_path.empty()) template_path = value;
                else if (option == "--input" && input.empty()) input = value;
                else if (option == "--output" && output.empty()) output = value;
                else if (option == "--batches" && batches.empty()) batches = value;
                else if (option == "--control" && control.empty()) control = value;
                else throw std::invalid_argument("unknown or repeated simulate option: " + option);
            }
            if (template_path.empty() || input.empty() || output.empty()) throw std::invalid_argument("simulate requires templates, input and output");
            if (!control.empty() && control != "native") throw std::invalid_argument("unknown control implementation: " + control);
            auto document = read_json(template_path);
            const markov::TemplateCatalog catalog(std::move(document.at("catalog")));
            const auto assets = markov::TemplateAsset::read_roles(catalog, document.at("roles"));
            const auto result =
                simulate(assets, read_json(input), batches.empty() ? nlohmann::json(nullptr) : read_json(batches), details, control == "native");
            std::vector<std::filesystem::path> inputs{ template_path, input };
            if (!batches.empty()) inputs.push_back(batches);
            write_json(output, result, inputs, !details);
            (control == "native" ? std::cerr : std::cout) << nlohmann::json{
                {          "scope",                                result.at("scope") },
                {         "status",                               result.at("status") },
                { "control_status",                       result.at("control_status") },
                {        "batches", result.at("structure").at("counts").at("batches") },
                {        "actions", result.at("structure").at("counts").at("actions") },
                {       "requests",                      result.at("requests").size() },
                {     "free_pages",                           result.at("free_pages") }
            }.dump(2) << '\n';
            return 0;
        }

        if (argc >= 2 && std::string(argv[1]) == "templates") {
            std::vector<std::pair<std::filesystem::path, std::filesystem::path>> sources;
            std::filesystem::path output;
            std::filesystem::path instances;
            for (int index = 2; index < argc; ++index) {
                const std::string option = argv[index];
                if (index + 1 == argc) throw std::invalid_argument("templates option has no value");
                const std::filesystem::path value = argv[++index];
                if (option == "--source") sources.emplace_back(value, std::filesystem::path{});
                else if (option == "--physical" && !sources.empty() && sources.back().second.empty()) sources.back().second = value;
                else if (option == "--output" && output.empty()) output = value;
                else if (option == "--instances" && instances.empty()) instances = value;
                else throw std::invalid_argument("unknown or repeated templates option: " + option);
            }
            if (sources.empty() || output.empty()) throw std::invalid_argument("templates requires sources and output");

            markov::TemplateLibrary library;
            std::vector<std::filesystem::path> inputs;
            for (const auto & [source, physical_path] : sources) {
                const markov::SourceGraph graph(read_json(source));
                const auto gaps = graph.summary().at("contract_gaps");
                if (!gaps.empty()) throw std::invalid_argument("source contract has gaps in " + source.string() + ": " + gaps.front().dump());
                inputs.push_back(source);

                const auto physical = physical_path.empty() ? nlohmann::json(nullptr) : read_json(physical_path);
                library.add(markov::SourceProjection(graph).document(physical_path.empty() ? nullptr : &physical));
                if (!physical_path.empty()) inputs.push_back(physical_path);
            }
            if (!instances.empty()) {
                library.add_instances(read_json(instances).at("instances"));
                inputs.push_back(instances);
            }
            const auto result = std::move(library).document();
            const markov::TemplateCatalog catalog(result.at("catalog"));
            markov::TemplateAsset::read_roles(catalog, result.at("roles"));
            write_json(output, result, inputs);
            std::cout << nlohmann::json{
                {        "definitions",        result.at("catalog").at("definitions").size() },
                {            "layouts",            result.at("catalog").at("layouts").size() },
                { "execution_patterns", result.at("catalog").at("execution_patterns").size() },
                {              "roles",                            result.at("roles").size() }
            }.dump(2) << '\n';
            return 0;
        }

        const bool inspect = argc == 4 && std::string(argv[1]) == "inspect" && std::string(argv[2]) == "--source";
        const bool with_physical = argc == 8 && std::string(argv[4]) == "--physical" && std::string(argv[6]) == "--output";
        const bool source_output = (argc == 6 && std::string(argv[4]) == "--output") || with_physical;
        const bool produce = source_output && std::string(argv[1]) == "produce" && std::string(argv[2]) == "--source";
        const bool expand = argc == 8 && std::string(argv[1]) == "expand" && std::string(argv[2]) == "--templates" && std::string(argv[4]) == "--target"
                            && std::string(argv[6]) == "--output";
        const bool composition = argc == 8 && std::string(argv[1]) == "compose" && std::string(argv[2]) == "--templates" && std::string(argv[4]) == "--target"
                                 && std::string(argv[6]) == "--output";
        if (!inspect && !produce && !expand && !composition) {
            std::cerr << "Usage: markov inspect --source observations.json\n"
                         "       markov produce --source observations.json [--physical physical.json] --output source-graph.json\n"
                         "       markov templates --source observations.json [--physical physical.json] [--source ...] [--instances roles.json] --output "
                         "templates.json\n"
                         "       markov expand --templates templates.json --target target.json --output region.json\n"
                         "       markov compose --templates templates.json --target calls.json --output graph.json\n"
                         "       markov record-batches --source rank.json [--source peer.json] --input serving.json --output batches.json\n"
                         "       markov simulate --templates templates.json --input serving.json --output result.json [--batches batches.json | --control "
                         "native] [--details]\n";
            return 2;
        }

        if (composition) {
            auto document = read_json(argv[3]);
            const markov::TemplateCatalog catalog(std::move(document.at("catalog")));
            const auto roles = markov::TemplateAsset::read_roles(catalog, document.at("roles"));
            const auto target = read_json(argv[5]);
            const auto & asset = select_role(roles, target);
            const auto result = compose(asset, target);
            write_json(argv[7], result, { argv[3], argv[5] });
            std::cout << nlohmann::json{
                {              "scope",               result.at("scope") },
                {        "invocations",  target.at("invocations").size() },
                {         "operations",   result.at("operations").size() },
                {             "values",       result.at("values").size() },
                {          "resources",           result.at("resources") },
                { "external_resources",  result.at("external_resources") },
                {       "dependencies", result.at("dependencies").size() }
            }.dump(2) << '\n';
            return 0;
        }

        if (expand) {
            auto document = read_json(argv[3]);
            const markov::TemplateCatalog catalog(std::move(document.at("catalog")));
            const auto roles = markov::TemplateAsset::read_roles(catalog, document.at("roles"));
            const auto target = read_json(argv[5]);
            const auto & asset = select_role(roles, target);
            auto instance = markov::TemplateInstance::select(asset,
                                                             target.at("role").get<std::string>(),
                                                             target.at("phase").get<std::string>(),
                                                             markov::Invocation::read(target));
            const auto result = instance.document();
            write_json(argv[7], result, { argv[3], argv[5] });
            std::cout << nlohmann::json{
                {                    "scope",                    result.at("scope") },
                {                     "role",                     result.at("role") },
                {            "state_changes",       instance.state_changes().size() },
                {              "input_ports",        instance.ports().inputs.size() },
                {             "output_ports",       instance.ports().outputs.size() },
                { "source_physical_coverage", result.at("source_physical_coverage") },
                {  "source_runtime_coverage",  result.at("source_runtime_coverage") },
                { "source_runtime_arguments", result.at("source_runtime_arguments") },
                {                   "layout",                   result.at("layout") },
                {               "operations",        result.at("operations").size() },
                {                   "values",            result.at("values").size() },
                {             "dependencies",      result.at("dependencies").size() }
            }.dump(2) << '\n';
            return 0;
        }

        // Source commands share contract inspection before projection or template construction.
        const markov::SourceGraph graph(read_json(argv[3]));
        const auto summary = graph.summary();
        const auto & gaps = summary.at("contract_gaps");
        if (inspect) {
            std::cout << summary.dump(2) << '\n';
            return gaps.empty() ? 0 : 1;
        }
        if (!gaps.empty()) throw std::invalid_argument("source contract has " + std::to_string(gaps.size()) + " gaps; first: " + gaps.front().dump());

        const auto physical = with_physical ? read_json(argv[5]) : nlohmann::json(nullptr);
        const auto result = markov::SourceProjection(graph).document(with_physical ? &physical : nullptr);

        if (with_physical) write_json(argv[7], result, { argv[3], argv[5] });
        else write_json(argv[5], result, { argv[3] });

        auto report = nlohmann::json{
            {   "operations",   result.at("operations").size() },
            {       "values",       result.at("values").size() },
            { "dependencies", result.at("dependencies").size() }
        };
        report["physical_coverage"] = result.at("physical_coverage").at("status");
        report["runtime_coverage"] = result.at("runtime_coverage").at("status");
        if (with_physical) report["unowned_work_kinds"] = result.at("physical_coverage").at("unowned").size();
        std::cout << report.dump(2) << '\n';
        return 0;
    }
    catch (const std::exception & error) {
        std::cerr << "markov: " << error.what() << '\n';
        return 1;
    }
}
