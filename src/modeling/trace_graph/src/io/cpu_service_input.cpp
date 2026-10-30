#include "markov/trace_graph/io/cpu_service_input.hpp"

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
    // Paired service evidence is provenance for model preparation, not a second
    // correction applied by the CPU simulator.
    if (!reference_io.is_null()) output << ",\n  \"reference_io\": " << reference_io.dump();
    output << ",\n  \"spans\": [";
    bool first = true;
    for (const auto & [lane, intervals] : cost.lanes()) {
        nlohmann::json row{
            {        "pid",    lane.first },
            {        "tid",   lane.second },
            {   "begin_us", uint64_t{ 0 } },
            {     "end_us", uint64_t{ 0 } },
            { "service_us", uint64_t{ 0 } }
        };
        auto & begin = row.at("begin_us");
        auto & end = row.at("end_us");
        auto & service = row.at("service_us");

        // A long capture can contain millions of intervals on the same lane.
        // Reuse the row's keys and thread identity, not its numeric values.
        for (const auto & span : intervals) {
            output << (first ? "\n    " : ",\n    ");
            first = false;
            begin = span.begin_us;
            end = span.end_us;
            service = span.service_us;
            output << row.dump();
        }
    }
    output << "\n  ]\n}\n";
    if (!output) throw std::runtime_error("Failed to write CPU service intervals");
}

namespace {

// Read only typed interval fields; a large capture never builds JSON row maps.
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
            root_field_ = value == "source_manifest" ? Field::Source : value == "spans" ? Field::Spans : Field::Ignored;
            if (root_field_ == Field::Spans) {
                if (saw_spans_) throw std::invalid_argument("CPU service spans is declared twice");
                saw_spans_ = true;
            }
        }
        else if (reading_spans_ && depth_ == 3) {
            field_ = value == "pid"          ? Field::Pid
                     : value == "tid"        ? Field::Tid
                     : value == "begin_us"   ? Field::Begin
                     : value == "end_us"     ? Field::End
                     : value == "service_us" ? Field::Service
                                             : Field::Ignored;
        }
        return true;
    }

    bool start_object(std::size_t) override {
        if (reading_spans_ && depth_ == 2) {
            lane_ = {};
            span_ = {};
            fields_ = 0;
        }
        else if (depth_) scalar(Field::Ignored);
        ++depth_;
        return true;
    }

    bool end_object() override {
        if (reading_spans_ && depth_ == 3) {
            if (fields_ != 31 || lane_.first.empty() || lane_.second.empty())
                throw std::invalid_argument("CPU service interval lacks a thread identity or integer coordinates");
            intervals[std::move(lane_)].push_back(span_);
        }
        --depth_;
        return true;
    }

    bool start_array(std::size_t) override {
        if (depth_ == 1 && root_field_ == Field::Spans) reading_spans_ = true;
        else scalar(Field::Ignored);
        ++depth_;
        return true;
    }

    bool end_array() override {
        if (reading_spans_ && depth_ == 2) reading_spans_ = false;
        --depth_;
        return true;
    }

    bool parse_error(std::size_t, const std::string &, const nlohmann::detail::exception & error) override {
        throw std::invalid_argument("Invalid CPU service JSON: " + std::string(error.what()));
    }

    bool complete() const { return saw_spans_ && depth_ == 0; }

private:
    enum class Field { Ignored, Source, Spans, Pid, Tid, Begin, End, Service, Number, String };

    bool scalar(Field type, uint64_t number = 0, std::string * text = nullptr) {
        if (!depth_ || (reading_spans_ && depth_ == 2)) throw std::invalid_argument("CPU service input requires an object and object spans");
        const auto field = depth_ == 1 ? root_field_ : reading_spans_ && depth_ == 3 ? field_ : Field::Ignored;
        if (field == Field::Ignored) return true;
        if (field == Field::Spans) throw std::invalid_argument("CPU service spans must be an array");
        if (field == Field::Source || field == Field::Pid || field == Field::Tid) {
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
        if (type != Field::Number) throw std::invalid_argument("CPU service coordinates and durations must be non-negative integers");
        if (field == Field::Begin) {
            span_.begin_us = number;
            fields_ |= 4;
        }
        else if (field == Field::End) {
            span_.end_us = number;
            fields_ |= 8;
        }
        else {
            span_.service_us = number;
            fields_ |= 16;
        }
        return true;
    }

    size_t depth_ = 0;
    Field root_field_ = Field::Ignored, field_ = Field::Ignored;
    bool reading_spans_ = false, saw_spans_ = false;
    core::CpuServiceCost::Lane lane_;
    core::CpuServiceCost::Span span_{};
    unsigned fields_ = 0;
};

} // namespace

core::CpuServiceCost read_cpu_service_cost(std::istream & input, const std::filesystem::path & source_manifest) {
    CpuServiceReader reader;
    if (!nlohmann::json::sax_parse(input, &reader) || !reader.complete()) throw std::invalid_argument("CPU service spans must be an array");
    if (reader.source_manifest.empty() || std::filesystem::weakly_canonical(reader.source_manifest) != std::filesystem::weakly_canonical(source_manifest))
        throw std::invalid_argument("CPU service intervals belong to a different source manifest");

    core::CpuServiceCost result;
    for (auto & [lane, selected] : reader.intervals) result.add_spans(lane, std::move(selected));
    return result;
}
} // namespace markov::trace_graph::io
