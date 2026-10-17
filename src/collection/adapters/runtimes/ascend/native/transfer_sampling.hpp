#pragma once

#include <cstdint>
#include <optional>

bool markov_transfer_sampling_enabled();

// Measure only the synchronous driver body inside a tracked native queue task.
class TransferDriverCopy {
public:
    TransferDriverCopy(std::uint64_t bytes, std::int32_t direction);
    void finish(std::int32_t result);

    TransferDriverCopy(const TransferDriverCopy &) = delete;
    TransferDriverCopy & operator=(const TransferDriverCopy &) = delete;

private:
    std::uint64_t bytes_;
    std::int32_t direction_;
    std::optional<std::int64_t> begin_;
};
