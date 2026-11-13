#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <unistd.h>

#include "markov/adapters/sglang/serving.hpp"
#include "markov/adapters/sglang/templates.hpp"
#include "markov/cli/execution_trace.hpp"
#include "markov/controller/control_peer.hpp"
#include "markov/execution/timeline.hpp"
#include "markov/execution/timing.hpp"
#include "markov/graph/observation.hpp"
#include "markov/graph/types.hpp"
#include "markov/production/projection.hpp"
#include "markov/production/templates.hpp"
#include "markov/templates/library.hpp"
#include "markov/templates/selection.hpp"

namespace {

class StreamPeer final : public markov::ControlPeer {
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

nlohmann::json read_json(const std::filesystem::path & path) {
    std::ifstream stream(path);
    if (!stream) throw std::runtime_error("cannot read input: " + path.string());
    stream.exceptions(std::ios::badbit);
    return nlohmann::json::parse(stream);
}

// Publishing a new artifact must neither expose a partial JSON nor replace an existing input or output.
class Publication {
public:
    explicit Publication(std::filesystem::path destination)
        : destination_(std::move(destination)),
          temporary_(destination_.string() + ".tmp." + std::to_string(getpid())) {
        if (!destination_.parent_path().empty()) std::filesystem::create_directories(destination_.parent_path());
        stream_.exceptions(std::ios::badbit | std::ios::failbit);
        stream_.open(temporary_, std::ios::out | std::ios::noreplace);
    }

    ~Publication() {
        std::error_code ignored;
        std::filesystem::remove(temporary_, ignored);
    }

    void write(const nlohmann::json & document) {
        stream_ << document << '\n';
        stream_.close();

        // A same-directory hard link is atomic and fails if the destination already exists, including symlinks.
        std::filesystem::create_hard_link(temporary_, destination_);
    }

private:
    std::filesystem::path destination_;
    std::filesystem::path temporary_;
    std::ofstream stream_;
};

constexpr std::string_view usage = "markov project <native-facts.json> <coarse.json> [--evidence]\n"
                                   "markov templates <coarse.json> <templates.json>\n"
                                   "markov link-templates <source-libraries.json> <output-directory>\n"
                                   "markov instantiate <templates.json> <invocation.json> <coarse.json>\n"
                                   "markov execute <coarse.json> <given-timing.json> <execution.json> [--trace <execution.jsonl>]\n"
                                   "markov serve <templates-directory> <task.json> <result.json> [--trace <execution.jsonl>]\n";

nlohmann::json execute(const markov::CoarseGraph & graph, const nlohmann::json & timing, markov::ExecutionObserver * observer) {
    std::vector<markov::ServiceDuration> work_ns;
    for (const auto & value : timing.at("work_ns").get_ref<const nlohmann::json::array_t &>()) {
        if (value.is_null()) work_ns.push_back(std::nullopt);
        else work_ns.push_back(markov::read_parameters({ { "service_ns", value } }).at("service_ns"));
    }
    std::vector<markov::BoundaryTime> boundaries;
    if (timing.contains("boundaries")) {
        for (const auto & value : timing.at("boundaries").get_ref<const nlohmann::json::array_t &>()) {
            const auto fields = markov::read_parameters(value);
            if (fields.size() != 3 || fields.at("event") < 0)
                throw std::invalid_argument("completion condition requires event, begin_ns and end_ns");
            boundaries.push_back({ static_cast<std::size_t>(fields.at("event")), { fields.at("begin_ns"), fields.at("end_ns") } });
        }
    }

    // This command executes supplied dependencies; it does not allocate state or choose serving actions.
    if (timing.at("initial_inputs") != "ready") throw std::invalid_argument("graph execution requires explicitly ready initial inputs");
    auto supplied = markov::GraphTiming::conditioned(graph, std::move(work_ns), std::move(boundaries));
    markov::Timeline timeline(graph.resources, observer);
    const auto events = timeline.insert(graph, supplied.work_ns, {}, supplied.boundaries);
    std::size_t completed = 0;
    while (timeline.advance()) ++completed;
    if (!timeline.done()) throw std::runtime_error("graph execution stalled with " + std::to_string(events.size() - completed) + " unfinished boundaries");
    const auto conditioned = std::ranges::count_if(supplied.work_ns, [](const auto & value) { return !value; });
    nlohmann::json result{
        {   "duration_basis", conditioned ? "given_service_and_completion_conditions" : "given_service_durations" },
        { "given_service_durations", supplied.work_ns.size() - static_cast<std::size_t>(conditioned) },
        { "completion_conditioned_work", conditioned },
        {   "initial_inputs",                                                                    "ready" },
        {     "completed_ns",                                                          timeline.now_ns() },
        { "completed_events",                                                                  completed },
        {   "work_intervals",                                                          graph.work.size() }
    };
    if (!supplied.boundaries.empty()) {
        result["boundary_events"] = supplied.boundaries.size();
        result["completion_time_selection"] = "earliest_causal_time_within_supplied_windows";
    }
    return result;
}

markov::GraphInvocation invocation(const nlohmann::json & source) {
    markov::GraphInvocation result;
    result.action = source.at("action").get<std::string>();
    result.condition = source.at("condition");
    result.parameters = markov::read_parameters(source.at("parameters"));
    result.identity = source.at("identity").get<std::string>();
    result.work = source.at("work");
    result.environment = source.at("environment");
    result.domain = markov::sglang::NativeTemplates::domain(result.environment);
    result.pools = source.at("pools").get<std::map<std::string, std::string>>();
    if (source.contains("signals")) result.signals = source.at("signals").get<std::map<std::string, std::string>>();
    for (const auto & [name, storage] : source.at("storage").get_ref<const nlohmann::json::object_t &>()) {
        const auto version = markov::read_parameters({
                                                         { "version", storage.at("version") }
        })
                                 .at("version");
        result.storage.emplace(name, markov::StorageBinding{ storage.at("identity").get<std::string>(), version });
    }
    for (const auto & resource : source.at("resources").get_ref<const nlohmann::json::array_t &>())
        result.resources.push_back(markov::Resource::read(resource));
    return result;
}

} // namespace

int main(int argc, char ** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--help") {
            std::cout << usage;
            return 0;
        }
        if (argc < 2) throw std::invalid_argument(std::string(usage));

        const std::string_view command = argv[1];
        const char * destination = argv[argc - 1];
        nlohmann::json output;
        if (command == "project" && (argc == 4 || (argc == 5 && std::string_view(argv[4]) == "--evidence"))) {
            destination = argv[3];
            const auto observation = markov::NativeObservation::read(read_json(argv[2]));
            markov::ProjectionEvidence evidence;
            output = markov::CoarseProjection::project(observation, argc == 5 ? &evidence : nullptr).document();
            if (argc == 5) output["evidence"] = evidence.document(observation);
        }
        else if (command == "templates" && argc == 4) {
            const auto source = markov::CoarseGraph::read(read_json(argv[2]));
            if (source.environment.at("framework") != "sglang") throw std::invalid_argument("source framework has no template recipe adapter");
            output = markov::sglang::NativeTemplates::compile(source).document();
        }
        else if (command == "link-templates" && argc == 4) {
            std::vector<markov::TemplateLibrary> libraries;
            for (const auto & path : read_json(argv[2])) libraries.push_back(markov::TemplateLibrary::read(read_json(path.get<std::string>())));
            if (libraries.empty()) throw std::invalid_argument("template linking requires a source library set");
            std::vector<markov::TemplateLibrary *> sources;
            for (auto & library : libraries) sources.push_back(&library);
            markov::TemplateLibrary::link_local_implementations(sources);
            const std::filesystem::path directory(argv[3]);
            if (!std::filesystem::create_directory(directory)) throw std::invalid_argument("template output directory already exists");
            try {
                for (const auto & library : libraries)
                    Publication(directory / (std::to_string(library.resources.at(0).rank) + ".json")).write(library.document());
            }
            catch (...) {

                // This invocation created the directory; a failed set must not leave apparently complete assets behind.
                std::error_code ignored;
                std::filesystem::remove_all(directory, ignored);
                throw;
            }
            std::cerr << "Created " << directory << '\n';
            return 0;
        }
        else if (command == "instantiate" && argc == 5) {
            const auto library = markov::TemplateLibrary::read(read_json(argv[2]));
            const auto call = invocation(read_json(argv[3]));
            const markov::TemplateSelection selection(library);
            output = library.instantiate(selection.select(call.action, call.condition, call.parameters), call).document();
        }
        else if (command == "serve" && (argc == 5 || (argc == 7 && std::string_view(argv[5]) == "--trace"))) {
            destination = argv[4];
            const auto task = read_json(argv[3]);
            for (const auto & [field, _] : task.items()) {
                if (field != "template_sources" && field != "instances" && field != "workload"
                    && field != "given_work_ns" && field != "execution_inputs")
                    throw std::invalid_argument("unknown serving input field: " + field);
            }
            std::map<std::size_t, markov::TemplateLibrary> libraries;
            for (const auto & source : task.at("template_sources")) {
                const auto index = source.get<std::size_t>();
                libraries.emplace(index, markov::TemplateLibrary::read(read_json(std::filesystem::path(argv[2]) / (std::to_string(index) + ".json"))));
            }
            const auto imports = markov::sglang::NativeTemplates::share_batch_transforms(libraries);

            // Auxiliary productions supply closed local bodies as well as complete batch actions.
            std::vector<markov::TemplateLibrary *> sources;
            for (auto & [_, library] : libraries) sources.push_back(&library);
            markov::TemplateLibrary::link_local_implementations(sources);
            std::unique_ptr<markov::ExecutionTrace> trace;
            if (argc == 7) trace = std::make_unique<markov::ExecutionTrace>(argv[6]);
            StreamPeer peer;
            output = markov::sglang::serve(libraries, task, peer, trace.get(), trace.get(), trace.get());
            output["template_imports"] = imports;
            if (trace) trace->finish();
        }
        else if (command == "execute" && (argc == 5 || (argc == 7 && std::string_view(argv[5]) == "--trace"))) {
            destination = argv[4];
            std::unique_ptr<markov::ExecutionTrace> trace;
            if (argc == 7) trace = std::make_unique<markov::ExecutionTrace>(argv[6]);
            output = execute(markov::CoarseGraph::read(read_json(argv[2])), read_json(argv[3]), trace.get());
            if (trace) trace->finish();
        }
        else throw std::invalid_argument(std::string(usage));

        Publication(destination).write(output);
        std::cerr << "Created " << destination << '\n';
        return 0;
    }
    catch (const std::exception & error) {
        std::cerr << "markov: " << error.what() << '\n';
        return 1;
    }
}
