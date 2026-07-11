/**
 * @file
 * @brief Lightweight process-local logging for the C++ TraceGraph backend.
 */
#pragma once

#include <cstdint>
#include <mutex>
#include <sstream>

namespace markov::trace_graph::core {

/** @brief Serializes complete log lines without imposing a logging dependency. */
class Logger {
public:
    /** @brief Severity threshold; lower values enable more verbose output. */
    enum Level : std::uint8_t { Debug = 0, Info = 1, Warn = 2, Error = 3, Off = 4 };

    static Logger & instance();

    /** @brief Sets the minimum severity emitted by this process. */
    void set_level(Level lv) { level_ = lv; }

    /** @brief Returns the current process-wide severity threshold. */
    [[nodiscard]] Level level() const { return level_; }

    /** @brief Tests a level before constructing expensive log arguments. */
    [[nodiscard]] bool enabled(Level lv) const { return level_ <= lv; }

    /**
     * @brief RAII builder that emits one complete line at destruction.
     *
     * The object buffers fragments locally and acquires the logger mutex only for
     * the final write. Moving a line transfers emission ownership exactly once.
     */
    class Line {
    public:
        /** @brief Creates an active line or a sink when the level is disabled. */
        Line(Level lv, bool active, std::mutex * mtx);
        ~Line();
        Line(const Line &) = delete;
        Line & operator=(const Line &) = delete;
        Line(Line && other) noexcept;
        Line & operator=(Line &&) = delete;

        /** @brief Appends one streamable fragment to the buffered line. */
        template <typename T> Line & operator<<(const T & v) {
            if (active_) ss_ << v;
            return *this;
        }

    private:
        std::ostringstream ss_;
        Level lv_;
        bool active_;
        std::mutex * mtx_;
    };

    /** @brief Creates a debug-level line. */
    [[nodiscard]] Line debug() { return Line(Debug, level_ <= Debug, &mtx_); }

    /** @brief Creates an info-level line. */
    [[nodiscard]] Line info() { return Line(Info, level_ <= Info, &mtx_); }

    /** @brief Creates a warning-level line. */
    [[nodiscard]] Line warn() { return Line(Warn, level_ <= Warn, &mtx_); }

    /** @brief Creates an error-level line. */
    [[nodiscard]] Line error() { return Line(Error, level_ <= Error, &mtx_); }

private:
    Logger() = default;

    Level level_ = Info;
    std::mutex mtx_;
};

} // namespace markov::trace_graph::core
