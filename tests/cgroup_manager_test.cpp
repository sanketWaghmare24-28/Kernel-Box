#include "../include/cgroups/cgroup_manager.hpp"
#include "../include/runtime/cgroup_cleanup_guard.hpp"

#include <filesystem>
#include <fstream>
#include <fcntl.h>
#include <iostream>
#include <unistd.h>

namespace fs = std::filesystem;

int main() {
    const fs::path temp = fs::temp_directory_path() / "kernelbox-cgroup-test";
    const fs::path base = temp / "base";
    const fs::path failed_base = temp / "failed";
    const std::string id = "0123456789ab";
    fs::remove_all(temp);

    kernelbox::cgroups::CgroupManager manager(base.string());
    if (!manager.create(id)) {
        std::cerr << "Cgroup creation failed\n";
        fs::remove_all(temp);
        return 1;
    }

    const fs::path group = base / id;
    std::ofstream(group / "cpu.max");
    std::ofstream(group / "memory.max");
    std::ofstream(group / "cgroup.procs");
    if (!manager.set_cpu_limit(id, "50000 100000") ||
        !manager.set_memory_limit(id, "67108864") ||
        !manager.add_process(id, 1234)) {
        std::cerr << "Cgroup controller operations failed unexpectedly\n";
        fs::remove_all(temp);
        return 1;
    }

    fs::remove(group / "cpu.max");
    fs::create_directory(group / "cpu.max");
    fs::remove(group / "memory.max");
    fs::create_directory(group / "memory.max");
    fs::remove(group / "cgroup.procs");
    fs::create_directory(group / "cgroup.procs");
    if (manager.set_cpu_limit(id, "50000 100000") ||
        manager.set_memory_limit(id, "67108864") ||
        manager.add_process(id, 1234) ||
        manager.add_process("../escape", 1234)) {
        std::cerr << "Cgroup operation failure was not propagated\n";
        fs::remove_all(temp);
        return 1;
    }

    fs::remove_all(group / "cpu.max");
    fs::remove_all(group / "memory.max");
    fs::remove_all(group / "cgroup.procs");
    if (!manager.create(id)) {
        std::cerr << "Cgroup recreation failed before log-open regression\n";
        fs::remove_all(temp);
        return 1;
    }
    {
        kernelbox::runtime::CgroupCleanupGuard cleanup(manager, id);
        const int log_fd = open(group.c_str(), O_WRONLY | O_APPEND | O_CREAT, 0644);
        if (log_fd >= 0) {
            close(log_fd);
            std::cerr << "Expected log open to fail for a directory path\n";
            fs::remove_all(temp);
            return 1;
        }
    }
    if (fs::exists(group)) {
        std::cerr << "Cgroup remained after simulated log-file open failure cleanup\n";
        fs::remove_all(temp);
        return 1;
    }

    fs::create_directories(failed_base / "cgroup.subtree_control");
    kernelbox::cgroups::CgroupManager failed_manager(failed_base.string());
    if (failed_manager.create(id)) {
        std::cerr << "Controller enable failure was not propagated\n";
        fs::remove_all(temp);
        return 1;
    }

    fs::remove_all(temp);
    return 0;
}
