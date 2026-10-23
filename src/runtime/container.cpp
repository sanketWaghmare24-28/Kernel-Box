// KernelBox — Container Runtime Implementation
// Orchestrates image, network, cgroups, process, and filesystem modules

#include "../../include/runtime/container.hpp"
#include "../../include/runtime/cgroup_cleanup_guard.hpp"
#include "../../include/image/image_manager.hpp"
#include "../../include/cgroups/cgroup_manager.hpp"
#include "../../include/network/network_manager.hpp"
#include "../../include/process/process_manager.hpp"
#include "../../include/filesystem/rootfs.hpp"
#include "../../include/logging/log_manager.hpp"
#include "../../include/common.hpp"

#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <thread>
#include <stdexcept>
#include <cstring>
#include <functional>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <sched.h>
#include <dirent.h>
#include <fcntl.h>

namespace fs = std::filesystem;

namespace kernelbox::runtime {

namespace {

class ScopeExit {
public:
    explicit ScopeExit(std::function<void()> cleanup)
        : cleanup_(std::move(cleanup)) {}
    ~ScopeExit() { if (active_) cleanup_(); }
    void release() { active_ = false; }

private:
    std::function<void()> cleanup_;
    bool active_ = true;
};

} // namespace

// ── State I/O ────────────────────────────────────────────────────────────────

ContainerState ContainerRuntime::load_state(const std::string& id) {
    std::string path = containers_dir_ + "/" + id + "/state.json";
    std::ifstream ifs(path);
    if (!ifs) throw std::runtime_error("State file not found for " + id);
    std::stringstream buf; buf << ifs.rdbuf();
    std::string j = buf.str();

    ContainerState s;
    s.id           = json::get_string(j, "id");
    s.name         = json::get_string(j, "name");
    s.image        = json::get_string(j, "image");
    s.pid          = json::get_int(j, "pid");
    s.status       = static_cast<ContainerStatus>(json::get_int(j, "status"));
    s.created_at   = json::get_string(j, "created_at");
    s.exit_code    = json::get_int(j, "exit_code");
    s.cpu_limit    = json::get_string(j, "cpu_limit");
    s.memory_limit = json::get_string(j, "memory_limit");
    return s;
}

bool ContainerRuntime::save_state(const ContainerState& s) {
    std::string path = containers_dir_ + "/" + s.id + "/state.json";
    std::ofstream ofs(path);
    if (!ofs) return false;
    ofs << "{\n"
        << "  \"id\": \"" << s.id << "\",\n"
        << "  \"name\": \"" << s.name << "\",\n"
        << "  \"image\": \"" << s.image << "\",\n"
        << "  \"pid\": " << s.pid << ",\n"
        << "  \"status\": " << static_cast<int>(s.status) << ",\n"
        << "  \"created_at\": \"" << s.created_at << "\",\n"
        << "  \"exit_code\": " << s.exit_code << ",\n"
        << "  \"cpu_limit\": \"" << s.cpu_limit << "\",\n"
        << "  \"memory_limit\": \"" << s.memory_limit << "\"\n"
        << "}\n";
    return true;
}

std::string ContainerRuntime::resolve_id(const std::string& id_or_name) {
    auto states = list();
    for (const auto& s : states) {
        if (s.id == id_or_name || s.name == id_or_name || s.id.find(id_or_name) == 0) {
            return s.id;
        }
    }
    throw std::runtime_error("Container not found: " + id_or_name);
}

// ── Lifecycle ────────────────────────────────────────────────────────────────

std::string ContainerRuntime::create(const std::string& image_tag, const std::string& name,
                                      const std::vector<std::pair<std::string,std::string>>& port_mappings,
                                      const std::string& cpu_limit,
                                      const std::string& memory_limit,
                                      [[maybe_unused]] bool detach) {
    if (!validate_name(name)) throw std::invalid_argument("Invalid container name");
    for (const auto& s : list()) {
        if (s.name == name) throw std::runtime_error("Container name conflict");
    }

    image::ImageManager im;
    auto manifest = im.get_manifest(image_tag);
    if (!manifest) throw std::runtime_error("Image not found: " + image_tag);

    std::string id = generate_id();
    std::string cdir = containers_dir_ + "/" + id;
    fs::create_directories(cdir);
    fs::create_directories(cdir + "/rootfs");

    if (!im.extract(image_tag, cdir + "/rootfs")) {
        throw std::runtime_error("Failed to extract image rootfs");
    }

    // Save port mappings
    if (!port_mappings.empty()) {
        std::ofstream pfs(cdir + "/ports.conf");
        for (const auto& p : port_mappings) pfs << p.first << ":" << p.second << "\n";
    }

    ContainerState s;
    s.id = id;
    s.name = name;
    s.image = image_tag;
    s.status = ContainerStatus::CREATED;
    s.created_at = current_timestamp();
    s.cpu_limit = cpu_limit;
    s.memory_limit = memory_limit;
    save_state(s);

    logging::LogManager lm;
    lm.init(id);

    log_info("Created container " + id + " (" + name + ")");
    return id;
}

bool ContainerRuntime::start(const std::string& id_or_name) {
    std::string id = resolve_id(id_or_name);
    ContainerState s = load_state(id);
    if (s.status == ContainerStatus::RUNNING) {
        log_warn("Container already running");
        return true;
    }

    image::ImageManager im;
    auto mf = im.get_manifest(s.image);
    if (!mf) throw std::runtime_error("Image manifest missing");

    cgroups::CgroupManager cm;
    if (!cm.create(id)) {
        throw std::runtime_error("Failed to create cgroup for container " + id);
    }
    CgroupCleanupGuard cleanup_cgroup(cm, id);
    if (!s.cpu_limit.empty() && !cm.set_cpu_limit(id, s.cpu_limit)) {
        throw std::runtime_error("Failed to set CPU limit for container " + id);
    }
    if (!s.memory_limit.empty() && !cm.set_memory_limit(id, s.memory_limit)) {
        throw std::runtime_error("Failed to set memory limit for container " + id);
    }

    network::NetworkManager nm;
    if (!nm.setup_bridge()) {
        throw std::runtime_error("Failed to set up container network bridge");
    }
    bool network_resources_owned = false;
    ScopeExit cleanup_network([&nm, &id, &network_resources_owned]() {
        if (network_resources_owned && !nm.cleanup(id)) {
            log_error("Failed to clean up network for " + id);
        }
    });

    std::string rootfs_path = containers_dir_ + "/" + id + "/rootfs";

    logging::LogManager lm;
    lm.init(id);
    int log_fd = open(lm.get_log_path(id).c_str(), O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (log_fd < 0) {
        throw std::runtime_error("Failed to open log file: " + std::string(strerror(errno)));
    }
    ScopeExit close_log([log_fd]() { close(log_fd); });

    process::ProcessManager pm;
    pid_t pid = -1;
    ScopeExit cleanup_process([&pm, &pid]() {
        if (pid > 0) {
            pm.send_signal(pid, SIGKILL);
            pm.wait_for(pid);
        }
    });
    int flags = CLONE_NEWNS | CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWNET;

    std::vector<std::string> cmd = mf->entrypoint;
    cmd.insert(cmd.end(), mf->args.begin(), mf->args.end());

    pid = pm.spawn(
        cmd, mf->env, mf->working_dir, s.name, flags,
        log_fd, log_fd,
        [rootfs_path]() {
            return filesystem::setup_mounts(rootfs_path) &&
                   filesystem::setup_dev(rootfs_path) &&
                   filesystem::do_pivot_root(rootfs_path);
        },
        [&cm, &id, &nm, &network_resources_owned, this](pid_t child_pid) {
            if (!cm.add_process(id, child_pid)) return false;
            if (!nm.create_veth_pair(id, child_pid)) return false;
            network_resources_owned = true;
            if (!nm.configure_container_network(id, child_pid)) return false;

            const std::string ports_path = containers_dir_ + "/" + id + "/ports.conf";
            std::ifstream pfs(ports_path);
            std::error_code ec;
            if (pfs.bad() || (fs::exists(ports_path, ec) && !pfs) || ec) return false;
            std::string line;
            const std::string cip = nm.get_container_ip(id);
            while (std::getline(pfs, line)) {
                const auto colon = line.find(':');
                if (colon == std::string::npos ||
                    !nm.add_port_mapping("0.0.0.0", line.substr(0, colon), cip,
                                         line.substr(colon + 1))) {
                    return false;
                }
            }
            return !pfs.bad();
        }
    );

    if (pid <= 0) {
        throw std::runtime_error("Failed to spawn container process");
    }

    s.pid = pid;
    s.status = ContainerStatus::RUNNING;
    if (!save_state(s)) {
        s.pid = 0;
        s.status = ContainerStatus::CREATED;
        throw std::runtime_error("Failed to save running state for container " + id);
    }

    log_info("Container " + id + " started (PID: " + std::to_string(pid) + ")");
    cleanup_process.release();
    cleanup_network.release();
    cleanup_cgroup.release();
    return true;
}

bool ContainerRuntime::stop(const std::string& id_or_name, int timeout) {
    std::string id = resolve_id(id_or_name);
    ContainerState s = load_state(id);
    if (s.status != ContainerStatus::RUNNING || s.pid <= 0) return true;

    process::ProcessManager pm;
    log_info("Stopping container " + id + "...");
    pm.send_signal(s.pid, SIGTERM);

    bool exited = false;
    for (int i = 0; i < timeout; ++i) {
        if (!pm.is_running(s.pid)) { exited = true; break; }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    if (!exited) {
        log_warn("Container did not stop in time. Sending SIGKILL.");
        pm.send_signal(s.pid, SIGKILL);
    }

    s.exit_code = pm.wait_for(s.pid);
    s.status = ContainerStatus::STOPPED;
    s.pid = 0;
    save_state(s);

    network::NetworkManager nm;
    nm.cleanup(id);
    cgroups::CgroupManager cm;
    cm.remove(id);

    log_info("Container stopped");
    return true;
}

bool ContainerRuntime::remove(const std::string& id_or_name) {
    std::string id = resolve_id(id_or_name);
    ContainerState s = load_state(id);
    if (s.status == ContainerStatus::RUNNING) {
        log_warn("Container is running. Stopping it first.");
        stop(id);
    }

    logging::LogManager lm;
    lm.cleanup(id);

    fs::remove_all(containers_dir_ + "/" + id);
    log_info("Container " + id + " removed");
    return true;
}

std::vector<ContainerState> ContainerRuntime::list() {
    std::vector<ContainerState> res;
    if (!fs::exists(containers_dir_)) return res;
    
    process::ProcessManager pm;
    for (const auto& entry : fs::directory_iterator(containers_dir_)) {
        if (!entry.is_directory()) continue;
        try {
            ContainerState s = load_state(entry.path().filename());
            // Auto-correct stale running states
            if (s.status == ContainerStatus::RUNNING && s.pid > 0 && !pm.is_running(s.pid)) {
                s.status = ContainerStatus::STOPPED;
                s.exit_code = -1;
                s.pid = 0;
                save_state(s);
                
                network::NetworkManager nm; nm.cleanup(s.id);
                cgroups::CgroupManager cm; cm.remove(s.id);
            }
            res.push_back(s);
        } catch (...) {}
    }
    return res;
}

std::optional<ContainerState> ContainerRuntime::get(const std::string& id_or_name) {
    try { return load_state(resolve_id(id_or_name)); }
    catch (...) { return std::nullopt; }
}

std::string ContainerRuntime::logs(const std::string& id_or_name) {
    std::string id = resolve_id(id_or_name);
    logging::LogManager lm;
    return lm.read(id);
}

} // namespace kernelbox::runtime
