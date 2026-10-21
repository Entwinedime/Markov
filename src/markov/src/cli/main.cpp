#include "markov/adapters/frameworks/ktransformers/collection_adapter.hpp"
#include "markov/adapters/frameworks/sglang/collection_adapter.hpp"
#include "markov/adapters/frameworks/sglang/template_adapter.hpp"
#include "markov/contracts/json_input.hpp"
#include "markov/workflow/cost_fitting_workflow.hpp"
#include "markov/workflow/extraction_workflow.hpp"
#include "markov/workflow/plan.hpp"
#include "markov/workflow/prediction_workflow.hpp"
#include "markov/workflow/project.hpp"
#include "markov/workflow/project_reader.hpp"
#include "markov/workflow/structure_comparison_workflow.hpp"

#include <array>
#include <exception>
#include <filesystem>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
constexpr std::string_view usage = "Usage: markov {plan|prepare-prediction} --project project.json\n"
                                   "       markov predict --project FILE --requirements-directory DIR --request-results FILE\n"
                                   "       markov compare-structure --project FILE --prediction-result FILE --validation-manifest FILE\n"
                                   "       markov fit-costs --performance-manifest performance_manifest.json [--unit "
                                   "all|kernel|communication|transfer|command|cpu]\n"
                                   "       markov evaluate-costs --model-candidate candidate.json --performance-manifest performance_manifest.json\n"
                                   "       markov prepare --project project.json --configuration ID --workload ID\n"
                                   "       markov prepare-representative --project project.json --configuration ID\n"
                                   "       markov prepare-target --project project.json --configuration ID --workload ID --budget-sec N "
                                   "[--purpose ports|structure]\n"
                                   "       markov prepare-sampling --project project.json --configuration ID --workload ID --measurement-requests FILE\n"
                                   "       markov prepare-performance --project project.json --prediction-result FILE --run-state FILE\n"
                                   "       markov prepare-communication --project project.json --configuration ID\n"
                                   "       markov extract --base-manifest profile_manifest.json "
                                   "[--output-directory DIR] [--work-directory DIR]\n";
}

int main(int argc, char ** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        std::cout << usage;
        return 0;
    }
    try {
        if (argc < 4 || argc % 2 != 0) throw std::invalid_argument(std::string(usage));
        const std::string_view command = argv[1];
        const std::set<std::string_view> commands{ "plan",
                                                   "predict",
                                                   "prepare-prediction",
                                                   "prepare-representative",
                                                   "prepare",
                                                   "prepare-target",
                                                   "prepare-sampling",
                                                   "prepare-communication",
                                                   "extract",
                                                   "fit-costs",
                                                   "evaluate-costs",
                                                   "prepare-performance",
                                                   "compare-structure" };
        if (!commands.contains(command)) throw std::invalid_argument(std::string(usage));

        const bool source_command = command == "extract";
        std::set<std::string_view> required{ source_command ? "--base-manifest" : "--project" };
        if (command == "fit-costs") required = { "--performance-manifest" };
        if (command == "evaluate-costs") required = { "--model-candidate", "--performance-manifest" };
        if (command == "prepare" || command == "prepare-representative" || command == "prepare-target" || command == "prepare-sampling"
            || command == "prepare-communication")
            required.insert("--configuration");
        if (command == "prepare" || command == "prepare-target" || command == "prepare-sampling") required.insert("--workload");
        if (command == "prepare-target") required.insert("--budget-sec");
        if (command == "prepare-sampling") required.insert("--measurement-requests");
        if (command == "prepare-performance") required.insert({ "--prediction-result", "--run-state" });
        if (command == "compare-structure") required.insert({ "--prediction-result", "--validation-manifest" });
        if (command == "predict") required.insert({ "--requirements-directory", "--request-results" });
        std::map<std::string_view, std::string> options;
        for (int index = 2; index < argc; index += 2) {
            const bool optional_unit = command == "fit-costs" && std::string_view(argv[index]) == "--unit";
            const bool optional_purpose = command == "prepare-target" && std::string_view(argv[index]) == "--purpose";
            const auto option = std::string_view(argv[index]);
            const bool output_path = source_command && (option == "--output-directory" || option == "--work-directory");
            if ((!required.contains(argv[index]) && !optional_unit && !optional_purpose && !output_path)
                || !options.emplace(argv[index], argv[index + 1]).second)
                throw std::invalid_argument("unknown or duplicate argument: " + std::string(argv[index]));
        }
        for (const auto key : required) {
            if (!options.contains(key)) throw std::invalid_argument("missing argument: " + std::string(key));
        }

        if (command == "fit-costs") {
            std::cout
                << markov::CostFittingWorkflow{}.run(options.at("--performance-manifest"), options.contains("--unit") ? options.at("--unit") : "all").dump(2)
                << '\n';
            return 0;
        }
        if (command == "evaluate-costs") {
            std::cout << markov::CostFittingWorkflow{}.evaluate(options.at("--model-candidate"), options.at("--performance-manifest")).dump(2) << '\n';
            return 0;
        }

        const markov::SGLangTemplateAdapter sglang;
        const std::array<const markov::TemplateAdapter *, 1> adapters{ &sglang };
        if (source_command) {
            const markov::ExtractionWorkflow workflow(adapters,
                                                      options.contains("--output-directory") ? options.at("--output-directory") : "",
                                                      options.contains("--work-directory") ? options.at("--work-directory") : "");
            const auto & manifest = options.at("--base-manifest");
            const auto result = workflow.run(manifest);
            std::cout << result.dump(2) << '\n';
            return 0;
        }
        const markov::SGLangCollectionAdapter sglang_collection;
        const markov::KTransformersCollectionAdapter kt_collection;
        const std::array<const markov::CollectionAdapter *, 2> collection_adapters{ &sglang_collection, &kt_collection };
        const auto project = markov::ProjectReader(collection_adapters).read(options.at("--project"));
        const markov::Planner planner(adapters, collection_adapters);
        nlohmann::json result;
        if (command == "predict") {
            // Resolve command-line outputs once before the workflow owns them.
            const markov::PredictionOutputs outputs{ std::filesystem::absolute(options.at("--requirements-directory")).lexically_normal(),
                                                     std::filesystem::absolute(options.at("--request-results")).lexically_normal() };
            result = markov::PredictionWorkflow(adapters).run(project, planner.collection_plan(project), outputs);
        }
        else if (command == "compare-structure")
            result = markov::StructureComparisonWorkflow(adapters, collection_adapters)
                         .run(project, options.at("--prediction-result"), options.at("--validation-manifest"));
        else if (command == "plan") result = planner.describe(project);
        else if (command == "prepare-prediction") result = planner.prepare_prediction(project);
        else if (command == "prepare-performance") {
            const auto state = markov::input::read_json(options.at("--run-state"));
            result = planner.prepare_performance(project, markov::input::read_json(options.at("--prediction-result")), state.at("performance_sampling"));
        }
        else {
            const auto & configuration = project.configuration(options.at("--configuration"));
            if (command == "prepare-communication") result = planner.prepare_communication(project, configuration);
            else if (command == "prepare-representative") result = planner.prepare_representative(project, configuration);
            else {
                const auto & workload = project.workload(options.at("--workload"));
                if (command == "prepare") result = planner.prepare_collection(project, configuration, workload.requests);
                else if (command == "prepare-target") {
                    const auto & value = options.at("--budget-sec");
                    std::size_t consumed;
                    const auto budget = std::stoll(value, &consumed);
                    if (consumed != value.size()) throw std::invalid_argument("--budget-sec must be an integer number of seconds");

                    result =
                        planner.prepare_target(project, configuration, workload, budget, options.contains("--purpose") ? options.at("--purpose") : "ports");
                }
                else { result = planner.prepare_sampling(project, configuration, workload, markov::input::read_json(options.at("--measurement-requests"))); }
            }
        }
        std::cout << result.dump(2) << '\n';
        return 0;
    }
    catch (const std::exception & error) {
        std::cerr << error.what() << '\n';
        return 2;
    }
}
