#include "source_dag/observation_writer.hpp"

#include <filesystem>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace markov::trace_graph::source_dag {

ObservationWriter::ObservationWriter(const std::string & path) {
    const auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent);
    stream_.open(path);
    if (!stream_) throw std::runtime_error("Cannot open operation observations: " + path);
}

void ObservationWriter::push_back(const nlohmann::json & observation, std::string_view operation_prefix) {
    if (!operation_prefix.empty() && !observation.at("operation").get_ref<const std::string &>().starts_with(operation_prefix)) return;
    stream_ << observation.dump() << '\n';
    if (!stream_) throw std::runtime_error("Cannot write operation observations");
    ++count_;
}

void ObservationWriter::append(const nlohmann::json & observations, std::string_view operation_prefix) {
    for (const auto & row : observations) push_back(row, operation_prefix);
}

void ObservationWriter::finish() {
    stream_.flush();
    if (!stream_) throw std::runtime_error("Cannot flush operation observations");
}

} // namespace markov::trace_graph::source_dag
