#include "trace_graph/activity_record.hpp"

#include <cctype>
#include <cstdint>
#include <sstream>

namespace TraceGraph {

int parse_int(const std::unordered_map<std::string, std::string> & args, const std::vector<std::string> & keys, const std::string & def) {
    for (const auto & k : keys) {
        if (args.count(k)) {
            try {
                return std::stoi(args.at(k));
            }
            catch (...) {
            }
        }
    }
    try {
        return std::stoi(def);
    }
    catch (...) {
        return -1;
    }
}

std::string ActivityRecord::escape_json(const std::string & input) {
    std::ostringstream oss;
    for (char c : input) {
        switch (c) {
        case '\\':
            oss << "\\\\";
            break;
        case '"':
            oss << "\\\"";
            break;
        case '\n':
            oss << "\\n";
            break;
        case '\r':
            oss << "\\r";
            break;
        case '\t':
            oss << "\\t";
            break;
        default:
            oss << c;
        }
    }
    return oss.str();
}

ActivityRecord::ActivityRecord(uint64_t ts_, uint64_t dur_, const std::string & cat_, const std::string & name_, const std::string & ph_)
    : ts(ts_),
      dur(dur_),
      cat(cat_),
      name(name_),
      ph(ph_) {}

void ActivityRecord::print(std::ostream & os) const {
    auto pid = parse_int(args, { "processId", "deviceId", "pid" }, "-1");
    auto tid = parse_int(args, { "threadId", "streamId", "tid" }, "-1");

    os << "  {\n";
    os << "    \"name\": \"" << escape_json(name) << "\",\n";
    if (!cat.empty()) os << "    \"cat\": \"" << escape_json(cat) << "\",\n";
    os << "    \"ph\": \"" << escape_json(ph) << "\",\n";
    os << "    \"ts\": " << ts << ",\n";
    if (dur > 0) os << "    \"dur\": " << dur << ",\n";
    os << "    \"pid\": " << pid << ",\n";
    os << "    \"tid\": " << tid << ",\n";
    os << "    \"args\": {";
    bool first = true;
    for (const auto & kv : args) {
        if (kv.first == "pid" || kv.first == "tid") continue;
        if (!first) os << ", ";
        os << "\"" << escape_json(kv.first) << "\": ";

        bool is_strict_num = false;
        if (!kv.second.empty() && (std::isdigit(kv.second[0]) || kv.second[0] == '-' || kv.second[0] == '+')) {
            try {
                size_t pos = 0;
                std::stod(kv.second, &pos);
                if (pos == kv.second.length()) { is_strict_num = true; }
            }
            catch (...) {
            }
        }

        if (is_strict_num) { os << kv.second; }
        else { os << "\"" << escape_json(kv.second) << "\""; }
        first = false;
    }
    os << "}\n  }";
}

VirtualRecord::VirtualRecord(const std::string & api_name, uint64_t ts, uint64_t dur, const std::unordered_map<std::string, std::string> & args_)
    : ActivityRecord(ts, dur, "Virtual", api_name) {
    args = args_;
}

} // namespace TraceGraph
