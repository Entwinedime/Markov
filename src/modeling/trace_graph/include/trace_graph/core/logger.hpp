#pragma once

#include <mutex>
#include <sstream>

namespace TraceGraph {

class Logger {
public:
    enum Level : int { DEBUG = 0, INFO = 1, WARN = 2, ERROR = 3, OFF = 4 };

    static Logger & instance();

    void set_level(Level lv) { level_ = lv; }
    Level level() const { return level_; }

    // RAII 日志行构造器：析构时一次性写出整行，避免多线程下半行日志交错。
    // 用户可见日志内容保持英文；这里的注释说明实现约束。
    class Line {
    public:
        Line(Level lv, bool active, std::mutex * mtx);
        ~Line();
        Line(Line &&) = default;
        Line & operator=(Line &&) = default;

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

    Line debug() { return Line(DEBUG, level_ <= DEBUG, &mtx_); }
    Line info() { return Line(INFO, level_ <= INFO, &mtx_); }
    Line warn() { return Line(WARN, level_ <= WARN, &mtx_); }
    Line error() { return Line(ERROR, level_ <= ERROR, &mtx_); }

private:
    Logger() = default;

    Level level_ = INFO;
    std::mutex mtx_;
};

} // namespace TraceGraph
