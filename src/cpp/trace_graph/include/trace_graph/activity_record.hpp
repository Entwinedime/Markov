#pragma once

#include <cstdint>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

namespace TraceGraph {

int parse_int(const std::unordered_map<std::string, std::string> & args, const std::vector<std::string> & keys, const std::string & def);

class ActivityRecord {
public:
    uint64_t ts;
    uint64_t dur;
    std::string cat;
    std::string name;
    std::string ph;
    std::unordered_map<std::string, std::string> args;

    ActivityRecord(uint64_t ts_, uint64_t dur_, const std::string & cat_, const std::string & name_, const std::string & ph_ = "X");
    virtual ~ActivityRecord() = default;

    static std::string escape_json(const std::string & input);
    virtual void print(std::ostream & os) const;
};

class VirtualRecord : public ActivityRecord {
public:
    VirtualRecord(const std::string & api_name, uint64_t ts, uint64_t dur, const std::unordered_map<std::string, std::string> & args_);
};

} // namespace TraceGraph
