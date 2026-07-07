/**
 * @file
 * @brief trace_graph C++ 后端的进程内日志实现。
 */
#include "markov/trace_graph/core/logger.hpp"

#include <algorithm>
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
    /**
     * @brief 全局 logger 只用于 C++ CLI 进程内日志；不跨动态库边界暴露。
     */
    static Logger logger;
    return logger;
}

namespace logger_detail {

bool use_color() {
    /**
     * @brief 默认只有 stderr 是 tty 时才输出颜色；也可以用 TRACE_GRAPH_COLOR 强制开关。
     */
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
    /**
     * @brief 用户可见日志保持英文 label，颜色只影响终端显示，不写入 JSON 输出。
     */
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
    /**
     * @brief 日志时间只用于人工调试，不参与性能预测或 validation。
     */
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1'000;
    std::ostringstream ss;
    ss << std::put_time(std::localtime(&t), "%H:%M:%S") << '.' << std::setfill('0') << std::setw(3) << ms.count();
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
    /**
     * @brief active=false 时仍允许调用方继续 << 拼接，但不会产生实际输出。
     */
    if (active_) ss_ << prefix(lv);
}

Logger::Line::~Line() {
    /**
     * @brief 析构时一次性加锁输出整行，避免多个日志调用在 stderr 上交错。
     */
    if (active_) {
        ss_ << "\n";
        std::lock_guard<std::mutex> lock(*mtx_);
        std::cerr << ss_.str() << std::flush;
    }
}

} // namespace markov::trace_graph::core
