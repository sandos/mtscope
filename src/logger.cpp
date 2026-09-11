#include "logger.h"

#include <ctime>

namespace {

std::string json_escape(const std::string& value) {
    std::string output;
    for (const char character : value) {
        if (character == '\\') output += "\\\\";
        else if (character == '"') output += "\\\"";
        else if (character == '\n') output += "\\n";
        else if (character == '\r') output += "\\r";
        else output += character;
    }
    return output;
}

} // namespace

void Logger::info(const std::string& source, const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.push_back({std::time(nullptr), source, message});
    if (entries_.size() > max_entries_) entries_.pop_front();
}

std::string Logger::json() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string result = "[";
    bool first = true;
    for (auto entry = entries_.rbegin(); entry != entries_.rend(); ++entry) {
        if (!first) result += ',';
        first = false;
        result += "{\"timestamp\":" + std::to_string(entry->timestamp) +
            ",\"source\":\"" + json_escape(entry->source) +
            "\",\"message\":\"" + json_escape(entry->message) + "\"}";
    }
    return result + "]";
}