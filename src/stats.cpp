#include "stats.h"

#include <fstream>
#include <limits>
#include <sstream>
#include <unistd.h>

namespace {

std::uint64_t process_cpu_ticks() {
    std::ifstream file("/proc/self/stat");
    std::string line;
    std::getline(file, line);
    const auto closing_parenthesis = line.rfind(')');
    if (closing_parenthesis == std::string::npos) return 0;
    std::istringstream stream(line.substr(closing_parenthesis + 1));
    char state;
    std::uint64_t ignored = 0;
    std::uint64_t user_ticks = 0;
    std::uint64_t system_ticks = 0;
    if (!(stream >> state)) return 0;
    for (int field = 4; field <= 13; ++field) {
        if (!(stream >> ignored)) return 0;
    }
    if (!(stream >> user_ticks >> system_ticks)) return 0;
    return user_ticks + system_ticks;
}

std::uint64_t process_ram_bytes() {
    std::ifstream file("/proc/self/status");
    std::string line;
    while (std::getline(file, line)) {
        std::istringstream stream(line);
        std::string key;
        std::uint64_t value = 0;
        if (!(stream >> key >> value)) continue;
        if (key == "VmRSS:") return value * 1024;
    }
    return 0;
}

} // namespace

std::string ResourceStats::json() {
    std::ifstream cpu_file("/proc/stat");
    std::string cpu_line;
    std::getline(cpu_file, cpu_line);
    std::istringstream cpu_stream(cpu_line);
    std::string label;
    std::uint64_t value = 0;
    std::uint64_t total = 0;
    std::uint64_t idle = 0;
    cpu_stream >> label;
    for (int index = 0; cpu_stream >> value; ++index) {
        total += value;
        if (index == 3 || index == 4) idle += value;
    }
    std::string host_cpu_usage = "null";
    if (has_previous_host_cpu_sample_ && total > previous_host_cpu_total_) {
        const auto total_delta = total - previous_host_cpu_total_;
        const auto idle_delta = idle - previous_host_cpu_idle_;
        host_cpu_usage = std::to_string(100.0 * static_cast<double>(total_delta - idle_delta) / total_delta);
    }
    if (total > 0) {
        previous_host_cpu_total_ = total;
        previous_host_cpu_idle_ = idle;
        has_previous_host_cpu_sample_ = true;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto current_process_cpu_ticks = process_cpu_ticks();
    std::string process_cpu_usage = "null";
    if (has_previous_process_cpu_sample_ && current_process_cpu_ticks >= previous_process_cpu_ticks_) {
        const auto elapsed = std::chrono::duration<double>(now - previous_process_cpu_sample_).count();
        const long ticks_per_second = sysconf(_SC_CLK_TCK);
        const long processors = sysconf(_SC_NPROCESSORS_ONLN);
        if (elapsed > 0 && ticks_per_second > 0 && processors > 0) {
            process_cpu_usage = std::to_string(100.0 * (current_process_cpu_ticks - previous_process_cpu_ticks_) / (ticks_per_second * elapsed * processors));
        }
    }
    if (current_process_cpu_ticks > 0) {
        previous_process_cpu_ticks_ = current_process_cpu_ticks;
        previous_process_cpu_sample_ = now;
        has_previous_process_cpu_sample_ = true;
    }

    std::ifstream memory_file("/proc/meminfo");
    std::string key;
    std::uint64_t memory_value = 0;
    std::uint64_t total_memory = 0;
    std::uint64_t available_memory = 0;
    while (memory_file >> key >> memory_value) {
        if (key == "MemTotal:") total_memory = memory_value * 1024;
        else if (key == "MemAvailable:") available_memory = memory_value * 1024;
        memory_file.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    }
    const std::string total_memory_json = total_memory > 0 ? std::to_string(total_memory) : "null";
    const std::string used_memory_json = total_memory > 0 && available_memory <= total_memory ? std::to_string(total_memory - available_memory) : "null";
    const auto process_ram = process_ram_bytes();
    const std::string process_ram_json = process_ram > 0 ? std::to_string(process_ram) : "null";
    return "\"host_cpu_usage_percent\":" + host_cpu_usage + ",\"host_ram_used_bytes\":" + used_memory_json + ",\"host_ram_total_bytes\":" + total_memory_json + ",\"process_cpu_usage_percent\":" + process_cpu_usage + ",\"process_ram_bytes\":" + process_ram_json;
}