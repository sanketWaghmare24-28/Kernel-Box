// KernelBox — Cgroups v2 Module Implementation
// Resource limits and usage statistics via /sys/fs/cgroup

#include "../../include/cgroups/cgroup_manager.hpp"
#include "../../include/common.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <cstring>
#include <cctype>
#include <unistd.h>

namespace fs = std::filesystem;

namespace kernelbox::cgroups {

namespace {

bool valid_container_id(const std::string& id) {
    if (id.size() != 12) return false;
    for (unsigned char c : id) {
        if (!std::isxdigit(c)) return false;
    }
    return true;
}

} // namespace

std::string CgroupManager::cgroup_path(const std::string& container_id) const {
    return base_path_ + "/" + container_id;
}

bool CgroupManager::write_file(const std::string& path, const std::string& value) {
    std::ofstream out(path);
    if (!out) {
        log_error("write " + path + ": " + strerror(errno));
        return false;
    }
    out << value;
    out.flush();
    if (!out) {
        log_error("write " + path + " (data): " + strerror(errno));
        return false;
    }
    return true;
}

std::string CgroupManager::read_file(const std::string& path) {
    std::ifstream in(path);
    if (!in) return "";
    std::stringstream buf;
    buf << in.rdbuf();
    std::string s = buf.str();
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s;
}

bool CgroupManager::create(const std::string& container_id) {
    if (!valid_container_id(container_id)) {
        log_error("Invalid container ID for cgroup: " + container_id);
        return false;
    }

    std::error_code ec;

    // Create the kernelbox parent cgroup if needed
    if (!fs::exists(base_path_, ec)) {
        if (ec || !fs::create_directories(base_path_, ec)) {
            log_error("mkdir " + base_path_ + ": " + ec.message());
            return false;
        }
    }
    if (ec || !fs::is_directory(base_path_, ec) || ec) {
        log_error("Cgroup base is not a directory: " + base_path_);
        return false;
    }

    // Enable cpu + memory + pids controllers for children
    if (!write_file(base_path_ + "/cgroup.subtree_control", "+cpu +memory +pids")) {
        log_error("Enable cgroup controllers under " + base_path_);
        return false;
    }

    // Create the per-container cgroup
    std::string path = cgroup_path(container_id);
    ec.clear();
    if (!fs::exists(path, ec)) {
        if (ec || !fs::create_directories(path, ec)) {
            log_error("mkdir " + path + ": " + ec.message());
            return false;
        }
    }
    if (ec || !fs::is_directory(path, ec) || ec) {
        log_error("Cgroup path is not a directory: " + path);
        return false;
    }

    log_info("Created cgroup for " + container_id);
    return true;
}

bool CgroupManager::set_cpu_limit(const std::string& container_id, const std::string& limit) {
    if (!valid_container_id(container_id)) return false;
    if (limit.empty()) return true;
    std::string path = cgroup_path(container_id) + "/cpu.max";
    if (!write_file(path, limit)) {
        log_error("set cpu.max for " + container_id);
        return false;
    }
    log_debug("CPU limit → " + limit + " for " + container_id);
    return true;
}

bool CgroupManager::set_memory_limit(const std::string& container_id, const std::string& limit) {
    if (!valid_container_id(container_id)) return false;
    if (limit.empty()) return true;
    std::string path = cgroup_path(container_id) + "/memory.max";
    if (!write_file(path, limit)) {
        log_error("set memory.max for " + container_id);
        return false;
    }
    log_debug("Memory limit → " + limit + " for " + container_id);
    return true;
}

bool CgroupManager::add_process(const std::string& container_id, pid_t pid) {
    if (!valid_container_id(container_id) || pid <= 0) return false;
    std::string path = cgroup_path(container_id) + "/cgroup.procs";
    if (!write_file(path, std::to_string(pid))) {
        log_error("add pid " + std::to_string(pid) + " to cgroup " + container_id);
        return false;
    }
    log_info("Added pid " + std::to_string(pid) + " to cgroup " + container_id);
    return true;
}

CgroupStats CgroupManager::get_stats(const std::string& container_id) {
    CgroupStats stats{};
    std::string base = cgroup_path(container_id);

    // CPU usage
    std::string cpu_stat = read_file(base + "/cpu.stat");
    std::istringstream css(cpu_stat);
    std::string key; uint64_t val;
    while (css >> key >> val) {
        if (key == "usage_usec") { stats.cpu_usage_usec = val; break; }
    }

    // Memory
    auto safe_ull = [](const std::string& s) -> uint64_t {
        try { return std::stoull(s); } catch (...) { return 0; }
    };
    stats.memory_current = safe_ull(read_file(base + "/memory.current"));
    std::string mem_max  = read_file(base + "/memory.max");
    stats.memory_limit   = (mem_max == "max") ? 0 : safe_ull(mem_max);

    // PIDs
    stats.pids_current   = safe_ull(read_file(base + "/pids.current"));

    return stats;
}

bool CgroupManager::remove(const std::string& container_id) {
    if (!valid_container_id(container_id)) return false;
    std::string path = cgroup_path(container_id);
    std::error_code ec;
    if (fs::exists(path, ec)) {
        if (rmdir(path.c_str()) != 0) {
            log_error("rmdir " + path + ": " + strerror(errno));
            return false;
        }
        log_info("Removed cgroup for " + container_id);
    }
    // Try to remove parent if empty (ignore errors)
    rmdir(base_path_.c_str());
    return true;
}

} // namespace kernelbox::cgroups
