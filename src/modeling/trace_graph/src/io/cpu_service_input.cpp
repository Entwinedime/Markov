#include "io/cpu_service_input.hpp"
#include "markov/trace_graph/core/numeric.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace markov::trace_graph::io {
void write_cpu_service_cost(std::ostream & output, const std::filesystem::path & source_manifest, const core::CpuServiceCost & cost,
                            const std::filesystem::path & light_manifest, const nlohmann::json & reference_io, const nlohmann::json & measurement_window) {
    output << "{\n  \"source_manifest\": " << nlohmann::json(source_manifest.generic_string()).dump();
    output << ",\n  \"light_manifest\": " << nlohmann::json(light_manifest.generic_string()).dump();
    output << ",\n  \"measurement_window\": " << measurement_window.dump();
    // Paired service evidence is provenance, not a second CPU correction.
    if (!reference_io.is_null()) output << ",\n  \"reference_io\": " << reference_io.dump();
    output << ",\n  \"lanes\": [";
    bool first_lane = true;
    for (const auto & [lane, intervals] : cost.lanes()) {
        if (intervals.empty()) continue;
        output << (first_lane ? "\n    " : ",\n    ") << "{\"pid\":" << nlohmann::json(lane.first).dump() << ",\"tid\":" << nlohmann::json(lane.second).dump()
               << ",\"intervals\":[";
        first_lane = false;
        bool first = true;
        uint64_t previous_end = 0;
        for (const auto & span : intervals) {
            if (!first) output << ',';
            first = false;
            output << '[' << span.begin_us - previous_end << ',' << span.end_us - span.begin_us << ',' << span.service_us << ']';
            previous_end = span.end_us;
        }
        output << "]}";
    }
    output << "\n  ]\n}\n";
    if (!output) throw std::runtime_error("Failed to write CPU service intervals");
}

namespace {

// Relative integer coordinates avoid repeating an absolute clock in every row.
class CpuServiceReader : public nlohmann::json_sax<nlohmann::json> {
public:
    using Json = nlohmann::json;
    std::string source_manifest;
    std::map<core::CpuServiceCost::Lane, std::vector<core::CpuServiceCost::Span>> intervals;

    bool null() override { return scalar(Field::Ignored); }
    bool boolean(bool) override { return scalar(Field::Ignored); }
    bool number_integer(number_integer_t value) override { return scalar(value < 0 ? Field::Ignored : Field::Number, static_cast<uint64_t>(value)); }
    bool number_unsigned(number_unsigned_t value) override { return scalar(Field::Number, value); }
    bool number_float(number_float_t, const string_t &) override { return scalar(Field::Ignored); }
    bool string(string_t & value) override { return scalar(Field::String, 0, &value); }
    bool binary(binary_t &) override { return scalar(Field::Ignored); }

    bool key(string_t & value) override {
        if (depth_ == 1) {
            root_field_ = value == "source_manifest" ? Field::Source : value == "lanes" ? Field::Lanes : Field::Ignored;
            if (root_field_ == Field::Lanes) {
                if (saw_lanes_) throw std::invalid_argument("CPU service lanes is declared twice");
                saw_lanes_ = true;
            }
        }
        else if (reading_lanes_ && depth_ == 3) {
            field_ = value == "pid" ? Field::Pid : value == "tid" ? Field::Tid : value == "intervals" ? Field::Intervals : Field::Ignored;
        }
        return true;
    }

    bool start_object(std::size_t) override {
        if (reading_lanes_ && depth_ == 2) {
            lane_ = {};
            spans_.clear();
            fields_ = 0;
            previous_end_ = 0;
        }
        else if (depth_) scalar(Field::Ignored);
        ++depth_;
        return true;
    }

    bool end_object() override {
        if (reading_lanes_ && depth_ == 3) {
            if (fields_ != 7 || lane_.first.empty() || lane_.second.empty())
                throw std::invalid_argument("CPU service lane needs a thread identity and integer intervals");
            if (!intervals.emplace(std::move(lane_), std::move(spans_)).second) throw std::invalid_argument("CPU service lane is declared twice");
        }
        --depth_;
        return true;
    }

    bool start_array(std::size_t) override {
        if (depth_ == 1 && root_field_ == Field::Lanes) reading_lanes_ = true;
        else if (reading_lanes_ && depth_ == 3 && field_ == Field::Intervals) {
            if (fields_ & 4) throw std::invalid_argument("CPU service intervals is declared twice");
            fields_ |= 4;
            reading_intervals_ = true;
        }
        else if (reading_intervals_ && depth_ == 4) {
            span_ = {};
            columns_ = 0;
        }
        else scalar(Field::Ignored);
        ++depth_;
        return true;
    }

    bool end_array() override {
        if (reading_intervals_ && depth_ == 5) {
            if (columns_ != 3) throw std::invalid_argument("CPU service interval needs gap_us, duration_us, service_us");
            spans_.push_back(span_);
            previous_end_ = span_.end_us;
        }
        else if (reading_intervals_ && depth_ == 4) reading_intervals_ = false;
        else if (reading_lanes_ && depth_ == 2) reading_lanes_ = false;
        --depth_;
        return true;
    }

    bool parse_error(std::size_t, const std::string &, const nlohmann::detail::exception & error) override {
        throw std::invalid_argument("Invalid CPU service JSON: " + std::string(error.what()));
    }

    bool complete() const { return saw_lanes_ && depth_ == 0; }

private:
    enum class Field { Ignored, Source, Lanes, Pid, Tid, Intervals, Number, String };

    bool scalar(Field type, uint64_t number = 0, std::string * text = nullptr) {
        if (!depth_ || (reading_lanes_ && depth_ == 2)) throw std::invalid_argument("CPU service input requires an object and object lanes");
        if (reading_intervals_) {
            if (depth_ != 5 || type != Field::Number || columns_ >= 3)
                throw std::invalid_argument("CPU service coordinates and durations must be three non-negative integers");
            if (columns_ == 0) span_.begin_us = core::checked_add_u64(previous_end_, number, "CPU interval start overflows uint64");
            else if (columns_ == 1) span_.end_us = core::checked_add_u64(span_.begin_us, number, "CPU interval end overflows uint64");
            else span_.service_us = number;
            ++columns_;
            return true;
        }
        const auto field = depth_ == 1 ? root_field_ : reading_lanes_ && depth_ == 3 ? field_ : Field::Ignored;
        if (field == Field::Ignored) return true;
        if (field == Field::Lanes || field == Field::Intervals) throw std::invalid_argument("CPU service lanes and intervals must be arrays");
        if (type != Field::String) throw std::invalid_argument("CPU service manifest and thread identities must be strings");
        if (field == Field::Source) source_manifest = *text;
        else if (field == Field::Pid) {
            lane_.first = *text;
            fields_ |= 1;
        }
        else {
            lane_.second = *text;
            fields_ |= 2;
        }
        return true;
    }

    size_t depth_ = 0, columns_ = 0;
    Field root_field_ = Field::Ignored, field_ = Field::Ignored;
    bool reading_lanes_ = false, reading_intervals_ = false, saw_lanes_ = false;
    core::CpuServiceCost::Lane lane_;
    core::CpuServiceCost::Span span_{};
    std::vector<core::CpuServiceCost::Span> spans_;
    uint64_t previous_end_ = 0;
    unsigned fields_ = 0;
};

} // namespace

core::CpuServiceCost read_cpu_service_cost(std::istream & input, const std::filesystem::path & source_manifest) {
    CpuServiceReader reader;
    if (!nlohmann::json::sax_parse(input, &reader) || !reader.complete())
        throw std::invalid_argument("CPU service needs grouped lanes; convert an expanded input with prepare-cpu-service --convert-input");
    if (reader.source_manifest.empty() || std::filesystem::weakly_canonical(reader.source_manifest) != std::filesystem::weakly_canonical(source_manifest))
        throw std::invalid_argument("CPU service intervals belong to a different source manifest");

    core::CpuServiceCost result;
    for (auto & [lane, selected] : reader.intervals)
        if (!selected.empty()) result.add_spans(lane, std::move(selected));
    return result;
}
} // namespace markov::trace_graph::io
