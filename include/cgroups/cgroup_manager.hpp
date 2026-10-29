// KernelBox — Cgroups v2 Module
// Manages resource limits (CPU, memory, PIDs) using the Linux cgroups v2 unified hierarchy

#pragma once
#include <string>
#include <cstdint>
#include <sys/types.h>

namespace kernelbox::cgroups {

struct CgroupStats {
    uint64_t cpu_usage_usec  = 0;
    uint64_t cpu_user_usec   = 0;
    uint64_t cpu_system_usec = 0;
    uint64_t memory_current  = 0;
    uint64_t memory_limit    = 0;
    uint64_t memory_peak     = 0;
    uint64_t oom_kills       = 0;
    uint64_t oom_count       = 0;
    uint64_t pids_current    = 0;
};

class CgroupManager {
public:
    explicit CgroupManager(std::string base_path = "/sys/fs/cgroup/kernelbox")
        : base_path_(std::move(base_path)) {}

    /// Detect whether the host system supports a unified cgroups v2 hierarchy.
    static bool is_cgroup_v2_supported();

    /// Check whether the current user has delegation/write permissions in cgroup v2.
    bool check_permissions();

    /// Create /sys/fs/cgroup/kernelbox/<container_id> and enable controllers.
    bool create(const std::string& container_id);

    /// Write CFS quota and period to cpu.max (e.g. "50000 100000").
    bool set_cpu_limit(const std::string& container_id, const std::string& limit);

    /// Write byte limit to memory.max.
    bool set_memory_limit(const std::string& container_id, const std::string& limit);

    /// Attach a process by writing its PID to cgroup.procs.
    bool add_process(const std::string& container_id, pid_t pid);

    /// Read CPU, memory, OOM events, and PID stats.
    CgroupStats get_stats(const std::string& container_id);

    /// Freeze (pause) or unfreeze (unpause) all processes in the container via cgroups v2 cgroup.freeze.
    bool freeze(const std::string& container_id, bool freeze_state);

    /// Remove the cgroup directory (rmdir — cgroups v2 requirement).
    bool remove(const std::string& container_id);

    std::string base_path() const { return base_path_; }

private:
    std::string base_path_;

    std::string cgroup_path(const std::string& container_id) const;
    static bool write_file(const std::string& path, const std::string& value);
    static std::string read_file(const std::string& path);
};

} // namespace kernelbox::cgroups
