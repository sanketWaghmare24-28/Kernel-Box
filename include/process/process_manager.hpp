// KernelBox — Process Module
// Spawns isolated container processes using clone(2) with namespace flags

#pragma once
#include <string>
#include <vector>
#include <functional>
#include <sys/types.h>

namespace kernelbox::process {

class ProcessManager {
public:
    /// Spawn an isolated child process using clone().
    /// clone_flags should include namespace flags (CLONE_NEWNS, CLONE_NEWPID, ...).
    /// before_child_setup runs in the parent while the child is blocked.
    /// pre_exec_setup runs in the child before execve; returning false prevents execution.
    pid_t spawn(const std::vector<std::string>& command,
                const std::vector<std::string>& env,
                const std::string& working_dir,
                const std::string& hostname,
                int clone_flags,
                int stdout_fd = -1,
                int stderr_fd = -1,
                std::function<bool()> pre_exec_setup = nullptr,
                std::function<bool(pid_t)> before_child_setup = nullptr);

    /// Block until pid exits. Returns exit code (or 128+signal).
    int wait_for(pid_t pid);

    /// Send a signal to the process.
    bool send_signal(pid_t pid, int signal);

    /// Check if the process is still alive.
    bool is_running(pid_t pid);
};

} // namespace kernelbox::process
