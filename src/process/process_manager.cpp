// KernelBox — Process Module Implementation
// Spawns isolated child processes using clone(2)

#include "../../include/process/process_manager.hpp"
#include "../../include/common.hpp"

#include <sched.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cstring>
#include <iostream>

namespace kernelbox::process {

// Data passed to the child function through clone()
struct ChildData {
    std::vector<std::string> command;
    std::vector<std::string> env;
    std::string working_dir;
    std::string hostname;
    int stdout_fd;
    int stderr_fd;
    std::function<void()> pre_exec_setup;
};

// This function runs in the cloned child process
static int child_fn(void* arg) {
    ChildData* data = static_cast<ChildData*>(arg);

    // 1. Set hostname (UTS namespace)
    if (!data->hostname.empty()) {
        if (sethostname(data->hostname.c_str(), data->hostname.length()) != 0) {
            fprintf(stderr, "sethostname: %s\n", strerror(errno));
        }
    }

    // 2. Redirect stdout/stderr if pipe fds were provided
    if (data->stdout_fd >= 0) {
        dup2(data->stdout_fd, STDOUT_FILENO);
        close(data->stdout_fd);
    }
    if (data->stderr_fd >= 0) {
        dup2(data->stderr_fd, STDERR_FILENO);
        close(data->stderr_fd);
    }

    // 3. Run pre-exec setup (mount rootfs, pivot_root, etc.)
    if (data->pre_exec_setup) {
        data->pre_exec_setup();
    }

    // 4. Change working directory
    if (!data->working_dir.empty()) {
        if (chdir(data->working_dir.c_str()) != 0) {
            fprintf(stderr, "chdir %s: %s\n", data->working_dir.c_str(), strerror(errno));
        }
    }

    // 5. Build argv and envp for execve
    std::vector<char*> c_args;
    for (const auto& a : data->command)
        c_args.push_back(const_cast<char*>(a.c_str()));
    c_args.push_back(nullptr);

    std::vector<char*> c_env;
    for (const auto& e : data->env)
        c_env.push_back(const_cast<char*>(e.c_str()));
    // Always provide a minimal PATH if no PATH in env
    bool has_path = false;
    for (const auto& e : data->env) {
        if (e.rfind("PATH=", 0) == 0) { has_path = true; break; }
    }
    std::string default_path = "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";
    if (!has_path) c_env.push_back(const_cast<char*>(default_path.c_str()));
    c_env.push_back(nullptr);

    // 6. Execute
    execve(c_args[0], c_args.data(), c_env.data());
    fprintf(stderr, "execve %s: %s\n", c_args[0], strerror(errno));
    _exit(1);
}

pid_t ProcessManager::spawn(const std::vector<std::string>& command,
                             const std::vector<std::string>& env,
                             const std::string& working_dir,
                             const std::string& hostname,
                             int clone_flags,
                             int stdout_fd,
                             int stderr_fd,
                             std::function<void()> pre_exec_setup) {
    // Allocate a 1 MiB stack for the child
    constexpr int STACK_SIZE = 1024 * 1024;
    void* stack = mmap(nullptr, STACK_SIZE, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
    if (stack == MAP_FAILED) {
        log_error("mmap stack: " + std::string(strerror(errno)));
        return -1;
    }
    void* stack_top = static_cast<char*>(stack) + STACK_SIZE;

    ChildData data;
    data.command        = command;
    data.env            = env;
    data.working_dir    = working_dir;
    data.hostname       = hostname;
    data.stdout_fd      = stdout_fd;
    data.stderr_fd      = stderr_fd;
    data.pre_exec_setup = std::move(pre_exec_setup);

    pid_t pid = clone(child_fn, stack_top, clone_flags | SIGCHLD, &data);
    if (pid == -1) {
        log_error("clone: " + std::string(strerror(errno)));
    }

    // Parent can unmap the stack — the child has its own copy (MAP_PRIVATE)
    munmap(stack, STACK_SIZE);
    return pid;
}

int ProcessManager::wait_for(pid_t pid) {
    int status;
    if (waitpid(pid, &status, 0) == -1) {
        log_error("waitpid: " + std::string(strerror(errno)));
        return -1;
    }
    if (WIFEXITED(status))   return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}

bool ProcessManager::send_signal(pid_t pid, int sig) {
    if (kill(pid, sig) == -1) {
        log_error("kill(" + std::to_string(pid) + ", " + std::to_string(sig) + "): " + strerror(errno));
        return false;
    }
    return true;
}

bool ProcessManager::is_running(pid_t pid) {
    if (kill(pid, 0) == -1) return (errno != ESRCH);
    return true;
}

} // namespace kernelbox::process
