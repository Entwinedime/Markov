/**
 * @file
 * @brief C++ trace_graph 后端的轻量日志设施。
 */
#pragma once

#include <mutex>
#include <sstream>

namespace markov::trace_graph::core {

/** @brief C++ TraceGraph CLI 进程内使用的轻量日志器。 */
class Logger {
public:
    /** @brief 日志级别；数值越小输出越详细。枚举值避免与全局 DEBUG 宏同名。 */
    enum Level : int { Debug = 0, Info = 1, Warn = 2, Error = 3, Off = 4 };

    static Logger & instance();

    /** @brief 设置当前进程的最小输出日志级别。 */
    void set_level(Level lv) { level_ = lv; }

    /** @brief 读取当前进程的最小输出日志级别。 */
    [[nodiscard]] Level level() const { return level_; }

    /**
     * @brief RAII 日志行构造器。
     *
     * 析构时一次性写出整行，避免多线程下半行日志交错。用户可见日志内容保持英文；
     * 这里的注释说明实现约束。
     */
    class Line {
    public:
        /** @brief 记录一行日志的临时对象；active=false 时只吞掉输出。 */
        Line(Level lv, bool active, std::mutex * mtx);
        ~Line();
        Line(Line &&) = default;
        Line & operator=(Line &&) = default;

        /** @brief 追加一个可流式输出的片段；最终在析构时一次性输出。 */
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

    /** @brief 构造 debug 级日志行。 */
    [[nodiscard]] Line debug() { return Line(Debug, level_ <= Debug, &mtx_); }

    /** @brief 构造 info 级日志行。 */
    [[nodiscard]] Line info() { return Line(Info, level_ <= Info, &mtx_); }

    /** @brief 构造 warn 级日志行。 */
    [[nodiscard]] Line warn() { return Line(Warn, level_ <= Warn, &mtx_); }

    /** @brief 构造 error 级日志行。 */
    [[nodiscard]] Line error() { return Line(Error, level_ <= Error, &mtx_); }

private:
    Logger() = default;

    Level level_ = Info;
    std::mutex mtx_;
};

} // namespace markov::trace_graph::core
