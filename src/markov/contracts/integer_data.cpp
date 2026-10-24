#include "markov/contracts/integer_data.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace markov {

void IntegerData::append(std::int64_t begin, std::int64_t count) {
    const auto maximum = std::numeric_limits<std::int64_t>::max();
    if (count < 0 || count > maximum - size_) throw std::invalid_argument("integer data size overflows or is negative");
    if (count == 0) return;
    if (begin > maximum - (count - 1)) throw std::invalid_argument("integer data range overflows");

    // Adjacent appends share one stored range instead of growing a token-by-token payload.
    if (!runs_.empty() && runs_.back().begin <= maximum - runs_.back().count && runs_.back().begin + runs_.back().count == begin) runs_.back().count += count;
    else runs_.push_back({ begin, count });

    size_ += count;
}

void IntegerData::append(IntegerData other) {
    for (const auto & run : other.runs_) append(run.begin, run.count);
}

IntegerData IntegerData::slice(std::int64_t offset, std::int64_t count) const {
    if (offset < 0 || count < 0 || offset > size_ || count > size_ - offset) throw std::out_of_range("integer data slice is out of range");

    IntegerData result;
    for (const auto & run : runs_) {
        if (count == 0) break;

        if (offset >= run.count) {
            offset -= run.count;
            continue;
        }

        const auto taken = std::min(count, run.count - offset);
        result.append(run.begin + offset, taken);
        count -= taken;
        offset = 0;
    }

    return result;
}

std::int64_t IntegerData::size() const { return size_; }

nlohmann::json IntegerData::document() const {
    auto result = nlohmann::json::array();
    for (const auto & run : runs_) result.push_back({ run.begin, run.count });

    return result;
}

} // namespace markov
