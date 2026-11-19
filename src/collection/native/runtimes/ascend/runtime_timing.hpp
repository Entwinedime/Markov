#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace markov::ascend {

enum class Observation { Off, Structure, Timing };
extern std::atomic<Observation> observation;
extern std::atomic_bool host_activity;

class RuntimeSample {
public:
    explicit RuntimeSample(const char * name, bool enabled = true);
    void enter();
    void finish(int result, std::uint32_t tasks = 0, std::size_t bytes = 0, int direction = -1) const;

private:
    const char * name_;
    std::int64_t before_;
    std::int64_t begin_ = 0;
};

} // namespace markov::ascend
