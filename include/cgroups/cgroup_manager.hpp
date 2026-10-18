// KernelBox — Cgroups v2 Module
// Manages container resource limits via cgroups v2

#pragma once
#include <string>
#include <cstdint>
#include <sys/types.h>

namespace kernelbox::cgroups {

/// Resource usage snapshot for a container.
struct CgroupStats {
    uint64_t cpu_usage_usec = 0;
    uint64_t memory_current = 0;
    uint64_t memory_limit   = 0;
    uint64_t pids_current   = 0;
};

class CgroupManager {
public:
    /// Create cgroup directory for a container under /sys/fs/cgroup/kernelbox/<id>.
    bool create(const std::string& container_id);

    /// Set CPU limit via cpu.max (e.g., "50000 100000" = 50%).
    bool set_cpu_limit(const std::string& container_id, const std::string& limit);

    /// Set memory limit via memory.max (bytes as string).
    bool set_memory_limit(const std::string& container_id, const std::string& limit);

    /// Write a PID into the cgroup's cgroup.procs file.
    bool add_process(const std::string& container_id, pid_t pid);

    /// Read CPU, memory, and PID stats.
    CgroupStats get_stats(const std::string& container_id);

    /// Remove the cgroup directory (rmdir — cgroups v2 requirement).
    bool remove(const std::string& container_id);

private:
    static constexpr const char* BASE_PATH = "/sys/fs/cgroup/kernelbox";
    std::string cgroup_path(const std::string& container_id) const;
    bool        write_file(const std::string& path, const std::string& value);
    std::string read_file(const std::string& path);
};

} // namespace kernelbox::cgroups
