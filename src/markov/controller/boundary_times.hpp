#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "markov/contracts/serving.hpp"
#include "markov/execution/timeline.hpp"

namespace markov {

// Measured action, notification and input returns constrain time, never batches, allocations or graph contents.
class ControlBoundaryTimes {
public:
    explicit ControlBoundaryTimes(const nlohmann::json & source);
    std::vector<BoundaryTime> take(const ServingAction & action, const CoarseGraph & graph);
    static nlohmann::json notification_identity(const nlohmann::json & value);
    std::optional<TimeWindow> notification(const nlohmann::json & identity);
    std::optional<std::int64_t> input(const nlohmann::json & identity);
    nlohmann::json finish() const;

private:
    struct PortTime {
        std::string port;
        Direction direction;
        std::int64_t time_ns;
    };

    static nlohmann::json identity(const std::string & group, const std::string & action, const Parameters & parameters,
                                   const nlohmann::json & work);

    struct ActionTimes {
        std::vector<PortTime> boundaries;
        std::size_t observed_calls;
        std::size_t predicted_calls = 0;
    };

    std::map<nlohmann::json, ActionTimes> timings_;
    struct NotificationTime {
        std::optional<TimeWindow> window;
        bool consumed = false;
    };
    std::map<nlohmann::json, NotificationTime> notifications_;
    struct InputTime {
        std::optional<std::int64_t> time_ns;
        std::size_t observed_calls = 0;
        std::size_t predicted_calls = 0;
    };
    std::map<nlohmann::json, InputTime> inputs_;
    std::size_t actions_ = 0;
    std::size_t events_ = 0;
};

} // namespace markov
