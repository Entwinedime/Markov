/** Same-source preparation with process-private target execution. */
#include "core/execution_metrics.hpp"
#include "frontend/cost_package.hpp"
#include "markov/trace_graph/cli/workflow.hpp"

#include "markov/trace_graph/cli/module_pipeline.hpp"
#include "markov/trace_graph/cli/options.hpp"
#include "markov/trace_graph/core/logger.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

namespace markov::trace_graph::cli {
namespace {

struct TargetRun {
    CliOptions options;
    ModulePipeline modules;
    std::filesystem::path log;
};

// Reap every owned child, including when spawning or result publication fails.
// The Python runner can also stop the whole inherited process group.
struct Children {
    std::vector<std::pair<pid_t, size_t>> active;

    ~Children() {
        for (const auto & [pid, index] : active) kill(pid, SIGTERM);
        for (const auto & [pid, index] : active)
            while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {}
    }
};

std::string output_path(const std::filesystem::path & directory, const char * name, bool enabled = true) {
    return enabled ? (directory / name).string() : std::string{};
}

std::vector<TargetRun> target_runs(const CliOptions & source_options, const nlohmann::json & plan, frontend::CostPackages & packages) {
    std::vector<TargetRun> runs;
    for (const auto & item : plan.at("runs")) {
        const std::filesystem::path directory(item.at("output_dir").get<std::string>());
        auto options = source_options;
        options.target_plan.clear();
        options.model_config = item.at("model_config").get<std::string>();
        options.outputs.run_summary = output_path(directory, "run_summary.json");
        options.outputs.graph = output_path(directory, "dag_chrome_trace.json", !source_options.outputs.graph.empty());
        options.outputs.structure_report = output_path(directory, "structure_report.json", !source_options.outputs.structure_report.empty());
#ifdef DEBUG
        options.outputs.model_summary = output_path(directory, "model_summary.json", !source_options.outputs.model_summary.empty());
        auto modules = ModulePipeline::from_config(options.model_config,
                                                   packages,
                                                   options.hicache_oracle_cost_replay,
                                                   options.hicache_phase_oracle_cost_replay,
                                                   options.hicache_canonical_observed_phase_scope,
                                                   options.hicache_static_replay);
#else
        auto modules = ModulePipeline::from_config(options.model_config, packages);
#endif
        runs.push_back({ std::move(options), std::move(modules), directory / "model.log" });
    }
    return runs;
}

int execute_target(TargetRun & run, PreparedSource & source, core::Logger & logger) {
    const int log = open(run.log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (log < 0) throw std::system_error(errno, std::generic_category(), "open target log");
    const bool redirected = dup2(log, STDOUT_FILENO) >= 0 && dup2(log, STDERR_FILENO) >= 0;
    const int error = errno;
    close(log);
    if (!redirected) throw std::system_error(error, std::generic_category(), "redirect target log");

    try {
        return run_prepared_workflow(run.options, run.modules, source, logger);
    }
    catch (const std::exception & failure) {
        logger.error() << failure.what();
        return 1;
    }
}

void publish_result(size_t index, int return_code, size_t inflight, const char * skip_reason = "") {
    std::cout << nlohmann::json{
        { "index", index }, { "return_code", return_code }, { "inflight", inflight }, { "skip_reason", skip_reason }
    }.dump() << '\n' << std::flush;
}

} // namespace

int run_prediction_batch(const CliOptions & options, core::Logger & logger) {
    std::ifstream input(options.target_plan);
    const auto plan = nlohmann::json::parse(input);
    const auto jobs = plan.at("jobs").get<int64_t>();
    if (jobs <= 0 || plan.at("runs").empty()) throw CliUsageError("target plan requires positive jobs and at least one run");
    const bool continue_on_error = plan.at("continue_on_error").get<bool>();
    core::ExecutionMetrics metrics;
    frontend::CostPackages packages;
    auto runs = target_runs(options, plan, packages);
    metrics.checkpoint("target_configs_and_costs_load");
    const auto preparation_started = std::chrono::steady_clock::now();
    auto source = prepare_source(options, true, std::ranges::any_of(runs, [](const auto & run) { return run.modules.uses_hicache_execution(); }));
    const auto preparation_sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - preparation_started).count();
    metrics.checkpoint("source_preparation");
    auto performance = metrics.report();
    performance["cost_packages_loaded"] = packages.size();
    std::cout << nlohmann::json{ { "event", "source_prepared" }, { "wall_seconds", preparation_sec }, { "performance", std::move(performance) } }.dump() << '\n' << std::flush;

    // Source evidence and observations are prepared before fork. Each child
    // owns its graph changes, execution clock and query statistics.
    Children children;
    children.active.reserve(std::min(static_cast<size_t>(jobs), runs.size()));
    size_t next = 0;
    bool failed = false;
    while (((!failed || continue_on_error) && next < runs.size()) || !children.active.empty()) {
        while ((!failed || continue_on_error) && next < runs.size() && children.active.size() < static_cast<size_t>(jobs)) {
            std::cout.flush();
            std::cerr.flush();
            const auto pid = fork();
            if (pid < 0) throw std::system_error(errno, std::generic_category(), "fork target execution");
            if (pid == 0) {
                try {
                    _exit(execute_target(runs[next], source, logger));
                }
                catch (const std::exception & error) {
                    logger.error() << error.what();
                    _exit(1);
                }
            }
            // Capacity is reserved before creating children; registering an
            // owned PID must not fail through a subsequent allocation.
            children.active.emplace_back(pid, next++);
            std::cout << nlohmann::json{ { "event", "target_started" }, { "index", next - 1 } }.dump() << '\n' << std::flush;
        }

        int status = 0;
        rusage usage{};
        pid_t completed;
        do { completed = wait4(-1, &status, 0, &usage); } while (completed < 0 && errno == EINTR);
        if (completed < 0) throw std::system_error(errno, std::generic_category(), "wait for target execution");
        const auto child = std::ranges::find_if(children.active, [&](const auto & entry) { return entry.first == completed; });
        const auto index = child->second;
        children.active.erase(child);
        const auto code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
        failed |= code != 0;
        std::cout << nlohmann::json{
            { "index", index }, { "return_code", code }, { "inflight", children.active.size() }, { "skip_reason", "" },
            { "performance", {
                { "target_cpu_seconds", usage.ru_utime.tv_sec + usage.ru_utime.tv_usec / 1e6 + usage.ru_stime.tv_sec + usage.ru_stime.tv_usec / 1e6 },
                { "target_process_peak_rss_bytes", static_cast<uint64_t>(usage.ru_maxrss) * 1'024 }
            } }
        }.dump() << '\n' << std::flush;
    }

    for (; next < runs.size(); ++next) publish_result(next, 0, 0, "not_started_after_failure");
    return failed ? 1 : 0;
}

} // namespace markov::trace_graph::cli
