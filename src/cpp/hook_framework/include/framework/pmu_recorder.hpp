#pragma once

#include <string>
#include <vector>

namespace HookFrameWork {

struct PmuSnapshot {
    bool valid = false;
    std::vector<std::string> event_names;
    std::vector<long long> counter_values;

    bool IsCompatibleWith(const PmuSnapshot & begin) const {
        return valid && begin.valid && event_names == begin.event_names && counter_values.size() == begin.counter_values.size();
    }

    std::vector<long long> DeltaFrom(const PmuSnapshot & begin) const {
        std::vector<long long> deltas;
        if (!IsCompatibleWith(begin)) { return deltas; }

        deltas.reserve(counter_values.size());
        for (size_t i = 0; i < counter_values.size(); ++i) { deltas.push_back(counter_values[i] - begin.counter_values[i]); }
        return deltas;
    }
};

class PmuRecorder {
public:
    static PmuRecorder & Get();
    bool ReadSnapshot(PmuSnapshot * snapshot);

private:
    PmuRecorder() = default;
};

} // namespace HookFrameWork
