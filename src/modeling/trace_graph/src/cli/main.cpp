/**
 * @file
 * @brief TraceGraph process entry point.
 */
#include "options.hpp"
#include "workflow.hpp"

#include "markov/trace_graph/core/logger.hpp"

#include <exception>
#include <iostream>

namespace {

void configure_logging(const markov::trace_graph::cli::CliOptions & options, markov::trace_graph::core::Logger & logger) {
    using markov::trace_graph::core::Logger;
    if (options.debug_logging) logger.set_level(Logger::Debug);
    else if (options.verbose_logging) logger.set_level(Logger::Info);
    else logger.set_level(Logger::Warn);
}

} // namespace

int main(int argc, char ** argv) {
    using namespace markov::trace_graph;
    try {
        auto options = cli::parse_cli_options(argc, argv);
        if (!options.has_value()) {
            cli::print_usage(argv[0]);
            return 0;
        }

        auto & logger = core::Logger::instance();
        configure_logging(*options, logger);
        return cli::run_workflow(*options, logger);
    }
    catch (const cli::CliUsageError & error) {
        std::cerr << "Error: " << error.what() << "\n\n";
        cli::print_usage(argv[0]);
        return 2;
    }
    catch (const std::exception & error) {
        core::Logger::instance().error() << error.what();
        return 1;
    }
}
