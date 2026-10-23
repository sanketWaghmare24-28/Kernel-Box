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
    int start_gate_fd;
    int start_gate_write_fd;
    int setup_status_fd;
    int setup_status_read_fd;
    std::function<bool()> pre_exec_setup;
};

// This function runs in the cloned child process
static int child_fn(void* arg) {
    ChildData* data = static_cast<ChildData*>(arg);

    close(data->start_gate_write_fd);
    char start = 0;
    ssize_t bytes;
    do {
        bytes = read(data->start_gate_fd, &start, 1);
    } while (bytes == -1 && errno == EINTR);
    close(data->start_gate_fd);
    close(data->setup_status_read_fd);
    if (bytes != 1 || start != 1) _exit(1);

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
    bool setup_ok = !data->pre_exec_setup || data->pre_exec_setup();

    // 4. Change working directory
    if (setup_ok && !data->working_dir.empty() && chdir(data->working_dir.c_str()) != 0) {
        fprintf(stderr, "chdir %s: %s\n", data->working_dir.c_str(), strerror(errno));
        setup_ok = false;
    }

    char setup_status = setup_ok ? 1 : 0;
    do {
        bytes = write(data->setup_status_fd, &setup_status, 1);
    } while (bytes == -1 && errno == EINTR);
    close(data->setup_status_fd);
    if (!setup_ok || bytes != 1) _exit(1);

    if (data->command.empty()) {
        fprintf(stderr, "execve: empty command\n");
        _exit(1);
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
                             std::function<bool()> pre_exec_setup,
                             std::function<bool(pid_t)> before_child_setup) {
    int start_gate[2] = {-1, -1};
    int setup_status[2] = {-1, -1};
    if (pipe(start_gate) != 0 || pipe(setup_status) != 0) {
        log_error("pipe startup handshake: " + std::string(strerror(errno)));
        if (start_gate[0] >= 0) close(start_gate[0]);
        if (start_gate[1] >= 0) close(start_gate[1]);
        if (setup_status[0] >= 0) close(setup_status[0]);
        if (setup_status[1] >= 0) close(setup_status[1]);
        return -1;
    }

    // Allocate a 1 MiB stack for the child
    constexpr int STACK_SIZE = 1024 * 1024;
    void* stack = mmap(nullptr, STACK_SIZE, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
    if (stack == MAP_FAILED) {
        log_error("mmap stack: " + std::string(strerror(errno)));
        close(start_gate[0]);
        close(start_gate[1]);
        close(setup_status[0]);
        close(setup_status[1]);
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
    data.start_gate_fd  = start_gate[0];
    data.start_gate_write_fd = start_gate[1];
    data.setup_status_fd = setup_status[1];
    data.setup_status_read_fd = setup_status[0];
    data.pre_exec_setup = std::move(pre_exec_setup);

    pid_t pid = clone(child_fn, stack_top, clone_flags | SIGCHLD, &data);
    if (pid == -1) {
        log_error("clone: " + std::string(strerror(errno)));
        close(start_gate[0]);
        close(start_gate[1]);
        close(setup_status[0]);
        close(setup_status[1]);
        munmap(stack, STACK_SIZE);
        return -1;
    }

    close(start_gate[0]);
    close(setup_status[1]);
    bool parent_setup_ok = !before_child_setup || before_child_setup(pid);
    char start = parent_setup_ok ? 1 : 0;
    ssize_t bytes;
    do {
        bytes = write(start_gate[1], &start, 1);
    } while (bytes == -1 && errno == EINTR);
    close(start_gate[1]);

    char child_setup_status = 0;
    ssize_t status_bytes;
    do {
        status_bytes = read(setup_status[0], &child_setup_status, 1);
    } while (status_bytes == -1 && errno == EINTR);
    close(setup_status[0]);

    if (!parent_setup_ok || bytes != 1 || status_bytes != 1 || child_setup_status != 1) {
        if (status_bytes == -1) {
            log_error("read child setup status: " + std::string(strerror(errno)));
            kill(pid, SIGKILL);
        }
        int status;
        while (waitpid(pid, &status, 0) == -1 && errno == EINTR) {}
        munmap(stack, STACK_SIZE);
        return -1;
    }

    // Parent can unmap the stack — the child has its own copy (MAP_PRIVATE).
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
