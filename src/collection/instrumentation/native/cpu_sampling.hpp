#pragma once

#include <cstdint>
#include <memory>

namespace markov::collection {

enum class CpuActivityKind { Scope, Operator, Enqueue, Worker, Submission, Query, Wait, Transfer, Function };

// Runtime adapters use the same exclusion clock for their own bookkeeping.
class CpuObserver {
public:
    explicit CpuObserver(bool force = false);
    ~CpuObserver();
    CpuObserver(const CpuObserver &) = delete;
    CpuObserver & operator=(const CpuObserver &) = delete;

private:
    bool active_;
};

// Shared by scope wrappers and runtime adapters. Names are observed identities;
// neither the recorder nor its clock knows framework or model semantics.
class CpuActivity {
public:
    CpuActivity(CpuActivityKind kind, const char * name, std::uint64_t correlation = 0);
    ~CpuActivity();
    CpuActivity(const CpuActivity &) = delete;
    CpuActivity & operator=(const CpuActivity &) = delete;

private:
    struct Record;
    std::unique_ptr<Record> record_;
};

class CpuScope {
public:
    CpuScope(std::uint64_t identity, CpuActivityKind kind, const char * name, std::uint64_t correlation = 0);
    ~CpuScope();
    CpuScope(const CpuScope &) = delete;
    CpuScope & operator=(const CpuScope &) = delete;

private:
    std::uint64_t previous_;
    CpuActivity activity_;
};

bool cpu_sampling_active();
bool cpu_sampling_enabled();
bool cpu_sampling_excluded();

} // namespace markov::collection
