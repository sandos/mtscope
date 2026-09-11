#pragma once

#include <cstddef>
#include <deque>
#include <mutex>
#include <string>

struct LogEntry {
    std::int64_t timestamp;
    std::string source;
    std::string message;
};

class Logger {
public:
    void info(const std::string& source, const std::string& message);
    std::string json() const;

private:
    static constexpr std::size_t max_entries_ = 500;
    mutable std::mutex mutex_;
    std::deque<LogEntry> entries_;
};