#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "markov/adapters/sglang/templates.hpp"
#include "markov/adapters/sglang/workers.hpp"
#include "markov/graph/graph.hpp"
#include "markov/templates/library.hpp"
#include "markov/templates/selection.hpp"

namespace {

nlohmann::json read_json(const char * path) {
    std::ifstream input(path);
    return nlohmann::json::parse(input);
}

// Only action metadata crosses this boundary; target work, ports and edges cannot enter the constructor.
class LocalGraphs {
public:
    LocalGraphs(const markov::TemplateLibrary & library, nlohmann::json environment)
        : library_(library), selection_(library), environment_(std::move(environment)), worker_(library, environment_) {}

    markov::CoarseGraph build(const markov::EntrySpecification & action, const markov::Parameters & parameters,
                             const nlohmann::json & work) const {
        auto entry = selection_.select(action.action, action.condition, parameters);
        worker_.bind(entry.frame, "local");
        markov::GraphInvocation call{ action.action, action.condition, parameters, library_.resources, work,
                                      "local/" + std::to_string(action.scope), environment_, library_.domain, {} };
        for (auto & resource : call.resources) resource.rank = environment_.at("parallel").at("rank").get<std::int64_t>();
        for (const auto & [name, _] : entry.frame.constants) call.parameters.erase(name);
        for (const auto & port : library_.definitions[entry.frame.definition].ports) {
            if (port.use == markov::PortUse::Signal) call.signals.emplace(port.name, call.identity + "/signal/" + port.name);
        }
        bind(entry.frame, call);
        return library_.instantiate(entry, call);
    }

private:
    void bind(const markov::TemplateFrame & frame, markov::GraphInvocation & call) const {

        // Local checks need symbolic external slots, not the target's allocation history or other graphs.
        for (const auto & tensor : frame.tensors) {
            if (tensor && tensor->external) call.storage.try_emplace(tensor->storage, markov::StorageBinding{ tensor->storage, 0 });
        }
        const auto & body = library_.definitions[frame.definition];
        for (const auto & access : body.kv_accesses) call.pools.try_emplace(access.pool, access.pool);
        for (const auto & access : body.slot_accesses) call.pools.try_emplace(access.pool, access.pool);
        for (const auto & child : frame.children) bind(child, call);
    }

    const markov::TemplateLibrary & library_;
    markov::TemplateSelection selection_;
    nlohmann::json environment_;
    markov::sglang::WorkerBindings worker_;
};

} // namespace

int main(int argc, char ** argv) {
    try {
        if (argc != 4) throw std::invalid_argument("markov_local_graphs <source-library.json> <observed-target.json> <local-graphs.jsonl>");
        const auto library = markov::TemplateLibrary::read(read_json(argv[1]));
        const auto observed = markov::CoarseGraph::read(read_json(argv[2]));

        // The adapter describes actual action inputs; it never compiles the target into a source library.
        const auto actions = markov::sglang::NativeTemplates::describe(observed);
        if (actions.entries.empty()) throw std::invalid_argument("target contains no native actions");
        const LocalGraphs graphs(library, observed.environment);
        std::ofstream output(argv[3], std::ios::out | std::ios::noreplace);
        output.exceptions(std::ios::failbit | std::ios::badbit);
        std::size_t built = 0;
        for (const auto & action : actions.entries) {
            nlohmann::json record{ { "kind", "local_graph" }, { "source_scope", action.scope } };
            try {
                const auto & input = actions.graph.scopes[action.scope].info;
                record["graph"] = graphs.build(action, input.parameters, input.work).document();
                ++built;
            }
            catch (const std::exception & error) {
                record["error"] = error.what();
            }
            output << record.dump() << '\n';
        }
        output << nlohmann::json{ { "kind", "local_summary" }, { "attempted", actions.entries.size() }, { "built", built } }.dump() << '\n';
        output.close();
        return built == actions.entries.size() ? 0 : 1;
    }
    catch (const std::exception & error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
