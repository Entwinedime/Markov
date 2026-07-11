/**
 * @file
 * @brief Process-local TraceGraph logger implementation.
 */
#include "markov/trace_graph/core/logger.hpp"

#include <algorithm>
#include <cstdlib>
#ifdef DEBUG
#include <chrono>
#include <ctime>
#include <iomanip>
#endif
#include <iostream>
#include <ranges>
#include <string_view>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace markov::trace_graph::core {

Logger & Logger::instance() {
    static Logger logger;
    return logger;
}

namespace logger_detail {

bool use_color() {
    // Resolve the environment once because all logger instances share stderr.
    static bool color = false;
    static std::once_flag init;
    std::call_once(init, [] {
        const char * env = std::getenv("TRACE_GRAPH_COLOR");
        if (env) {
            constexpr std::string_view false_values[] = { "0", "no", "off", "false" };
            const std::string_view value(env);
            color = std::ranges::find(false_values, value) == std::end(false_values);
        }
        else {
#ifdef _WIN32
            color = false;
#else
            color = isatty(STDERR_FILENO);
#endif
        }
    });
    return color;
}

const char * ansi(Logger::Level lv) {
    if (!use_color()) return "";
    switch (lv) {
    case Logger::Error:
        return "\x1b[1;31m";
    case Logger::Warn:
        return "\x1b[33m";
    case Logger::Info:
        return "\x1b[32m";
    case Logger::Debug:
        return "\x1b[2;36m";
    default:
        return "\x1b[0m";
    }
}

const char * reset() { return use_color() ? "\x1b[0m" : ""; }

const char * label(Logger::Level lv) {
    switch (lv) {
    case Logger::Error:
        return "ERROR";
    case Logger::Warn:
        return "WARN ";
    case Logger::Info:
        return "INFO ";
    case Logger::Debug:
        return "DEBUG";
    default:
        return "?????";
    }
}

#ifdef DEBUG
std::string timestamp() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1'000;
    std::tm local_time{};
#ifdef _WIN32
    localtime_s(&local_time, &t);
#else
    localtime_r(&t, &local_time);
#endif
    std::ostringstream ss;
    ss << std::put_time(&local_time, "%H:%M:%S") << '.' << std::setfill('0') << std::setw(3) << ms.count();
    return ss.str();
}
#endif

std::string prefix(Logger::Level lv) {
#ifdef DEBUG
    return std::string(ansi(lv)) + "[" + label(lv) + " " + timestamp() + "] " + reset();
#else
    return std::string(ansi(lv)) + "[" + label(lv) + "] " + reset();
#endif
}

} // namespace logger_detail

using logger_detail::prefix;

Logger::Line::Line(Level lv, bool active, std::mutex * mtx) : lv_(lv), active_(active), mtx_(mtx) {
    if (active_) ss_ << prefix(lv);
}

Logger::Line::Line(Line && other) noexcept : ss_(std::move(other.ss_)), lv_(other.lv_), active_(other.active_), mtx_(other.mtx_) {
    other.active_ = false;
    other.mtx_ = nullptr;
}

Logger::Line::~Line() {
    if (active_ && mtx_ != nullptr) {
        ss_ << "\n";
        std::lock_guard<std::mutex> lock(*mtx_);
        std::cerr << ss_.str() << std::flush;
    }
}

} // namespace markov::trace_graph::core
