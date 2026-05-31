#include "trace_graph/trace_parser.hpp"
#include "trace_graph/logger.hpp"

#include <cctype>
#include <cstdint>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace TraceGraph {

namespace {

size_t find_matching_array(const std::string & text, size_t open_pos) {
    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (size_t i = open_pos; i < text.size(); ++i) {
        char c = text[i];
        if (in_string) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') in_string = false;
            continue;
        }
        if (c == '"') in_string = true;
        else if (c == '[') depth++;
        else if (c == ']') {
            depth--;
            if (depth == 0) return i;
        }
    }
    return std::string::npos;
}

} // namespace

void TraceParser::skip_ws() {
    while (p < end && static_cast<unsigned char>(*p) <= ' ') p++;
}

std::string_view TraceParser::parse_string() {
    if (p >= end || *p != '"') return {};
    p++;
    const char * start = p;
    while (p < end && *p != '"') {
        if (*p == '\\') p++;
        p++;
    }
    std::string_view res(start, p - start);
    if (p < end) p++;
    return res;
}

std::string_view TraceParser::parse_primitive() {
    const char * start = p;
    while (p < end && *p != ',' && *p != '}' && *p != ']' && static_cast<unsigned char>(*p) > ' ') { p++; }
    return std::string_view(start, p - start);
}

void TraceParser::skip_value() {
    skip_ws();
    if (p >= end) return;
    if (*p == '"') { parse_string(); }
    else if (*p == '{') {
        p++;
        int depth = 1;
        while (p < end && depth > 0) {
            if (*p == '"') { parse_string(); }
            else if (*p == '{') { depth++; }
            else if (*p == '}') { depth--; }
            else { p++; }
        }
    }
    else if (*p == '[') {
        p++;
        int depth = 1;
        while (p < end && depth > 0) {
            if (*p == '"') { parse_string(); }
            else if (*p == '[') { depth++; }
            else if (*p == ']') { depth--; }
            else { p++; }
        }
    }
    else { parse_primitive(); }
}

void TraceParser::parse(const std::string & filename, std::vector<std::unique_ptr<ActivityRecord>> & records) {
    std::ifstream ifs(filename, std::ios::binary | std::ios::ate);
    if (!ifs.is_open()) {
        Logger::instance().error() << "Failed to open trace file: " << filename;
        return;
    }
    size_t size = ifs.tellg();
    ifs.seekg(0, std::ios::beg);
    std::string buffer(size, '\0');
    if (!ifs.read(&buffer[0], size)) return;

    size_t parse_start = 0;
    size_t parse_end = buffer.size();
    size_t trace_events_key = buffer.find("\"traceEvents\"");
    if (trace_events_key != std::string::npos) {
        size_t array_start = buffer.find('[', trace_events_key);
        if (array_start != std::string::npos) {
            size_t array_end = find_matching_array(buffer, array_start);
            if (array_end != std::string::npos) {
                parse_start = array_start + 1;
                parse_end = array_end;
            }
        }
    }

    p = buffer.data() + parse_start;
    end = buffer.data() + parse_end;

    while (p < end) {
        skip_ws();
        if (p >= end) break;

        if (*p != '{') {
            p++;
            continue;
        }
        p++;

        std::string name, cat, ph;
        double ts = 0, dur = 0;
        std::string pid = "-1", tid = "-1";
        std::unordered_map<std::string, std::string> args;
        bool is_valid_event = false;

        while (p < end) {
            skip_ws();
            if (p >= end || *p == '}') {
                if (p < end) p++;
                break;
            }

            if (*p == '"') {
                std::string_view key = parse_string();
                skip_ws();
                if (p < end && *p == ':') p++;
                skip_ws();

                if (key == "name") { name = std::string(parse_string()); }
                else if (key == "cat") { cat = std::string(parse_string()); }
                else if (key == "ph") {
                    ph = std::string(parse_string());
                    is_valid_event = true;
                }
                else if (key == "ts") {
                    if (*p == '"') ts = std::stod(std::string(parse_string()));
                    else ts = std::stod(std::string(parse_primitive()));
                }
                else if (key == "dur") {
                    if (*p == '"') dur = std::stod(std::string(parse_string()));
                    else dur = std::stod(std::string(parse_primitive()));
                }
                else if (key == "pid") {
                    if (*p == '"') pid = std::string(parse_string());
                    else pid = std::string(parse_primitive());
                }
                else if (key == "tid") {
                    if (*p == '"') tid = std::string(parse_string());
                    else tid = std::string(parse_primitive());
                }
                else if (key == "args" && *p == '{') {
                    p++;
                    while (p < end) {
                        skip_ws();
                        if (p >= end || *p == '}') {
                            if (p < end) p++;
                            break;
                        }
                        if (*p == '"') {
                            std::string_view arg_key = parse_string();
                            skip_ws();
                            if (p < end && *p == ':') p++;
                            skip_ws();
                            std::string_view arg_val;
                            if (*p == '"') arg_val = parse_string();
                            else arg_val = parse_primitive();
                            args[std::string(arg_key)] = std::string(arg_val);
                        }
                        skip_ws();
                        if (p < end && *p == ',') p++;
                    }
                }
                else { skip_value(); }
            }
            else { p++; }

            skip_ws();
            if (p < end && *p == ',') p++;
        }

        if (is_valid_event && (ph == "X" || ph == "B" || ph == "E" || ph == "s" || ph == "t" || ph == "M") && (name != "Free") && (name != "Computing")
            && (name != "Communication") && (name != "Communication(Not Overlapped)")) {
            auto record = std::make_unique<ActivityRecord>(static_cast<uint64_t>(ts), static_cast<uint64_t>(dur), cat, name, ph);
            record->args = std::move(args);
            record->args["pid"] = pid;
            record->args["tid"] = tid;
            records.push_back(std::move(record));
        }
    }
}

void parse_trace_json(const std::string & filename, std::vector<std::unique_ptr<ActivityRecord>> & records) {
    TraceParser parser;
    parser.parse(filename, records);
}

} // namespace TraceGraph
