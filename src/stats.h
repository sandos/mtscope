#pragma once

#include <chrono>
#include <cstdint>
#include <string>

class ResourceStats {
public:
    std::string json();

private:
    std::uint64_t previous_host_cpu_total_ = 0;
    std::uint64_t previous_host_cpu_idle_ = 0;
    std::uint64_t previous_process_cpu_ticks_ = 0;
    std::chrono::steady_clock::time_point previous_process_cpu_sample_;
    bool has_previous_host_cpu_sample_ = false;
    bool has_previous_process_cpu_sample_ = false;
};