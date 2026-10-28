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
#include <vector>
#include <set>
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
    s.id               = json::get_string(j, "id");
    s.name             = json::get_string(j, "name");
    s.image            = json::get_string(j, "image");
    s.pid              = json::get_int(j, "pid");
    s.start_time_ticks = json::get_uint64(j, "start_time_ticks");
    s.status           = static_cast<ContainerStatus>(json::get_int(j, "status"));
    s.created_at       = json::get_string(j, "created_at");
    s.started_at       = json::get_string(j, "started_at");
    s.finished_at      = json::get_string(j, "finished_at");
    s.exit_code        = json::get_int(j, "exit_code");
    s.cpu_limit        = json::get_string(j, "cpu_limit");
    s.memory_limit     = json::get_string(j, "memory_limit");
    s.error_message    = json::get_string(j, "error_message");
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
        << "  \"start_time_ticks\": " << s.start_time_ticks << ",\n"
        << "  \"status\": " << static_cast<int>(s.status) << ",\n"
        << "  \"created_at\": \"" << s.created_at << "\",\n"
        << "  \"started_at\": \"" << s.started_at << "\",\n"
        << "  \"finished_at\": \"" << s.finished_at << "\",\n"
        << "  \"exit_code\": " << s.exit_code << ",\n"
        << "  \"cpu_limit\": \"" << s.cpu_limit << "\",\n"
        << "  \"memory_limit\": \"" << s.memory_limit << "\",\n"
        << "  \"error_message\": \"" << s.error_message << "\"\n"
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
    if (!validate_name(name)) throw std::invalid_argument("Invalid container name: " + name);
    for (const auto& s : list()) {
        if (s.name == name) throw std::runtime_error("Container name conflict: '" + name + "' already exists");
    }

    // Validate port mappings format and check for conflicting host ports
    std::set<std::string> requested_host_ports;
    for (const auto& p : port_mappings) {
        int hp = 0, cp = 0;
        try {
            hp = std::stoi(p.first);
            cp = std::stoi(p.second);
        } catch (...) {
            throw std::invalid_argument("Port mapping values must be numeric: " + p.first + ":" + p.second);
        }
        if (hp <= 0 || hp > 65535 || cp <= 0 || cp > 65535) {
            throw std::invalid_argument("Port mapping out of range (1-65535): " + p.first + ":" + p.second);
        }
        if (!requested_host_ports.insert(p.first).second) {
            throw std::invalid_argument("Duplicate host port requested in port mappings: " + p.first);
        }
    }

    // Check host port conflict across existing containers
    std::error_code ec;
    if (fs::exists(containers_dir_, ec)) {
        for (const auto& entry : fs::directory_iterator(containers_dir_, ec)) {
            if (!entry.is_directory()) continue;
            std::string ports_file = entry.path().string() + "/ports.conf";
            std::ifstream pfs(ports_file);
            std::string line;
            while (std::getline(pfs, line)) {
                auto col = line.find(':');
                if (col != std::string::npos) {
                    std::string existing_hp = line.substr(0, col);
                    if (requested_host_ports.count(existing_hp)) {
                        throw std::runtime_error("Host port " + existing_hp + " is already allocated to container " +
                                                 entry.path().filename().string());
                    }
                }
            }
        }
    }

    image::ImageManager im;
    auto manifest = im.get_manifest(image_tag);
    if (!manifest) throw std::runtime_error("Image not found: " + image_tag);

    std::string id = generate_id();
    std::string cdir = containers_dir_ + "/" + id;
    fs::create_directories(cdir);
    fs::create_directories(cdir + "/rootfs");

    ScopeExit rollback_dir([&cdir]() {
        fs::remove_all(cdir);
    });

    if (!im.extract(image_tag, cdir + "/rootfs")) {
        throw std::runtime_error("Failed to extract image rootfs for image: " + image_tag);
    }

    // Save port mappings
    if (!port_mappings.empty()) {
        std::ofstream pfs(cdir + "/ports.conf");
        for (const auto& p : port_mappings) pfs << p.first << ":" << p.second << "\n";
    }

    ContainerState s;
    s.id           = id;
    s.name         = name;
    s.image        = image_tag;
    s.status       = ContainerStatus::CREATED;
    s.created_at   = current_timestamp();
    s.cpu_limit    = cpu_limit;
    s.memory_limit = memory_limit;
    if (!save_state(s)) {
        throw std::runtime_error("Failed to save initial container metadata");
    }

    logging::LogManager lm;
    lm.init(id);

    rollback_dir.release();
    log_info("Created container " + id + " (" + name + ")");
    return id;
}

bool ContainerRuntime::start(const std::string& id_or_name) {
    std::string id = resolve_id(id_or_name);
    ContainerState s = load_state(id);
    if (s.status == ContainerStatus::RUNNING) {
        log_warn("Container " + id_or_name + " is already running");
        return true;
    }
    if (s.status == ContainerStatus::PAUSED) {
        throw std::runtime_error("Container " + id_or_name + " is paused. Use 'unpause' to resume.");
    }
    if (s.status == ContainerStatus::REMOVING) {
        throw std::runtime_error("Container " + id_or_name + " is currently being removed.");
    }

    image::ImageManager im;
    auto mf = im.get_manifest(s.image);
    if (!mf) {
        s.status = ContainerStatus::FAILED;
        s.error_message = "Image manifest missing for: " + s.image;
        save_state(s);
        throw std::runtime_error(s.error_message);
    }

    cgroups::CgroupManager cm;
    if (!cm.is_cgroup_v2_supported()) {
        log_warn("Unified cgroups v2 not detected; resource limits may not be fully enforced");
    }

    if (!cm.create(id)) {
        s.status = ContainerStatus::FAILED;
        s.error_message = "Failed to create cgroup for container " + id;
        save_state(s);
        throw std::runtime_error(s.error_message);
    }
    CgroupCleanupGuard cleanup_cgroup(cm, id);

    if (!s.cpu_limit.empty() && !cm.set_cpu_limit(id, s.cpu_limit)) {
        s.status = ContainerStatus::FAILED;
        s.error_message = "Failed to set CPU limit: " + s.cpu_limit;
        save_state(s);
        throw std::runtime_error(s.error_message);
    }
    if (!s.memory_limit.empty() && !cm.set_memory_limit(id, s.memory_limit)) {
        s.status = ContainerStatus::FAILED;
        s.error_message = "Failed to set memory limit: " + s.memory_limit;
        save_state(s);
        throw std::runtime_error(s.error_message);
    }

    network::NetworkManager nm;
    if (!nm.setup_bridge()) {
        s.status = ContainerStatus::FAILED;
        s.error_message = "Failed to set up container network bridge kb0";
        save_state(s);
        throw std::runtime_error(s.error_message);
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
        s.status = ContainerStatus::FAILED;
        s.error_message = "Failed to open log file: " + std::string(strerror(errno));
        save_state(s);
        throw std::runtime_error(s.error_message);
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
        s.status = ContainerStatus::FAILED;
        s.error_message = "Failed to spawn container process via clone";
        save_state(s);
        throw std::runtime_error(s.error_message);
    }

    s.pid              = pid;
    s.start_time_ticks = pm.get_start_time(pid);
    s.started_at       = current_timestamp();
    s.status           = ContainerStatus::RUNNING;
    s.error_message    = "";
    if (!save_state(s)) {
        s.pid = 0;
        s.status = ContainerStatus::FAILED;
        s.error_message = "Failed to persist running state";
        throw std::runtime_error(s.error_message);
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
    if (s.status == ContainerStatus::STOPPED || s.status == ContainerStatus::CREATED || s.status == ContainerStatus::FAILED) {
        log_info("Container " + id + " is not running (status: " + status_to_string(s.status) + ")");
        return true;
    }

    cgroups::CgroupManager cm;
    if (s.status == ContainerStatus::PAUSED) {
        // Must unfreeze to let SIGTERM/SIGKILL be processed by kernel
        cm.freeze(id, false);
    }

    process::ProcessManager pm;
    log_info("Stopping container " + id + " (PID " + std::to_string(s.pid) + ")...");
    if (s.pid > 0 && pm.is_running(s.pid, s.start_time_ticks)) {
        pm.send_signal(s.pid, SIGTERM, s.start_time_ticks);

        bool exited = false;
        for (int i = 0; i < timeout; ++i) {
            if (!pm.is_running(s.pid, s.start_time_ticks)) { exited = true; break; }
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }

        if (!exited) {
            log_warn("Container did not stop within " + std::to_string(timeout) + "s. Sending SIGKILL.");
            pm.send_signal(s.pid, SIGKILL, s.start_time_ticks);
        }

        s.exit_code = pm.wait_for(s.pid);
    } else {
        s.exit_code = 0;
    }

    s.finished_at = current_timestamp();
    s.status = (s.exit_code == 0) ? ContainerStatus::STOPPED : ContainerStatus::FAILED;
    s.pid = 0;
    save_state(s);

    network::NetworkManager nm;
    nm.cleanup(id);
    cm.remove(id);
    filesystem::cleanup_rootfs(containers_dir_ + "/" + id + "/rootfs");

    log_info("Container " + id + " stopped (exit code: " + std::to_string(s.exit_code) + ")");
    return true;
}

bool ContainerRuntime::restart(const std::string& id_or_name, int timeout) {
    std::string id = resolve_id(id_or_name);
    ContainerState s = load_state(id);
    if (s.status == ContainerStatus::RUNNING || s.status == ContainerStatus::PAUSED) {
        stop(id, timeout);
    }
    return start(id);
}

bool ContainerRuntime::remove(const std::string& id_or_name, bool force) {
    std::string id = resolve_id(id_or_name);
    ContainerState s = load_state(id);
    if (s.status == ContainerStatus::RUNNING || s.status == ContainerStatus::PAUSED) {
        if (!force) {
            throw std::runtime_error("Cannot remove running container " + id_or_name +
                                     ". Stop the container first or use --force (-f).");
        }
        log_warn("Forced removal of running container " + id + ". Stopping first.");
        stop(id, 2);
    }

    s.status = ContainerStatus::REMOVING;
    save_state(s);

    network::NetworkManager nm;
    nm.cleanup(id);
    cgroups::CgroupManager cm;
    cm.remove(id);
    filesystem::cleanup_rootfs(containers_dir_ + "/" + id + "/rootfs");

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
            ContainerState s = load_state(entry.path().filename().string());
            // Auto-correct stale running states (e.g. after crash, SIGKILL, or system restart)
            if ((s.status == ContainerStatus::RUNNING || s.status == ContainerStatus::PAUSED) &&
                s.pid > 0 && !pm.is_running(s.pid, s.start_time_ticks)) {
                s.status = ContainerStatus::FAILED;
                s.exit_code = 137;
                s.pid = 0;
                s.finished_at = current_timestamp();
                s.error_message = "Container process terminated unexpectedly";
                save_state(s);

                network::NetworkManager nm; nm.cleanup(s.id);
                cgroups::CgroupManager cm; cm.remove(s.id);
                filesystem::cleanup_rootfs(containers_dir_ + "/" + s.id + "/rootfs");
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

int ContainerRuntime::exec(const std::string& id_or_name, const std::vector<std::string>& command) {
    std::string id = resolve_id(id_or_name);
    ContainerState s = load_state(id);
    if (s.status != ContainerStatus::RUNNING) {
        log_error("Container " + id_or_name + " is not running (status: " + status_to_string(s.status) + ")");
        return 1;
    }

    process::ProcessManager pm;
    if (!pm.is_running(s.pid, s.start_time_ticks)) {
        log_error("Container process " + std::to_string(s.pid) + " is no longer running");
        return 1;
    }

    // Attach to container cgroup
    cgroups::CgroupManager cm;
    std::string cg_procs = cm.base_path() + "/" + id + "/cgroup.procs";
    int cgroup_fd = open(cg_procs.c_str(), O_WRONLY);

    const char* ns_types[] = {"uts", "ipc", "mnt", "pid", "net"};
    int ns_flags[] = {CLONE_NEWUTS, CLONE_NEWIPC, CLONE_NEWNS, CLONE_NEWPID, CLONE_NEWNET};
    int fds[5];

    for (int i = 0; i < 5; ++i) {
        std::string path = "/proc/" + std::to_string(s.pid) + "/ns/" + ns_types[i];
        fds[i] = open(path.c_str(), O_RDONLY);
        if (fds[i] < 0) {
            log_error("Failed to open " + path + ": " + strerror(errno));
            if (cgroup_fd >= 0) close(cgroup_fd);
            for (int j = 0; j < i; ++j) close(fds[j]);
            return 1;
        }
    }

    for (int i = 0; i < 5; ++i) {
        if (setns(fds[i], ns_flags[i]) != 0) {
            log_error("setns " + std::string(ns_types[i]) + ": " + strerror(errno));
            if (cgroup_fd >= 0) close(cgroup_fd);
            for (int j = i; j < 5; ++j) close(fds[j]);
            return 1;
        }
        close(fds[i]);
    }

    pid_t child = fork();
    if (child == -1) {
        log_error("fork for exec failed: " + std::string(strerror(errno)));
        if (cgroup_fd >= 0) close(cgroup_fd);
        return 1;
    }

    if (child == 0) {
        if (cgroup_fd >= 0) close(cgroup_fd);
        if (chdir("/app") != 0) {
            if (chdir("/") != 0) {}
        }

        setenv("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", 0);
        setenv("TERM", "xterm-256color", 0);

        std::vector<char*> argv;
        for (const auto& arg : command) {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argv.push_back(nullptr);

        execvp(argv[0], argv.data());
        std::cerr << "execvp " << argv[0] << ": " << strerror(errno) << "\n";
        _exit(127);
    }

    if (cgroup_fd >= 0) {
        std::string pid_str = std::to_string(child) + "\n";
        (void)write(cgroup_fd, pid_str.c_str(), pid_str.size());
        close(cgroup_fd);
    }

    int status = 0;
    waitpid(child, &status, 0);
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}

std::string ContainerRuntime::inspect(const std::string& id_or_name) {
    std::string id = resolve_id(id_or_name);
    ContainerState s = load_state(id);
    network::NetworkManager nm;
    std::string ip = nm.get_container_ip(id);

    std::vector<std::string> ports;
    std::ifstream pfs(containers_dir_ + "/" + id + "/ports.conf");
    std::string line;
    while (std::getline(pfs, line)) {
        if (!line.empty()) ports.push_back(line);
    }

    cgroups::CgroupManager cm;
    auto stats = cm.get_stats(id);

    std::stringstream ss;
    ss << "{\n";
    ss << "  \"id\": \"" << s.id << "\",\n";
    ss << "  \"name\": \"" << s.name << "\",\n";
    ss << "  \"image\": \"" << s.image << "\",\n";
    ss << "  \"status\": \"" << status_to_string(s.status) << "\",\n";
    ss << "  \"pid\": " << s.pid << ",\n";
    ss << "  \"start_time_ticks\": " << s.start_time_ticks << ",\n";
    ss << "  \"created_at\": \"" << s.created_at << "\",\n";
    ss << "  \"started_at\": \"" << s.started_at << "\",\n";
    ss << "  \"finished_at\": \"" << s.finished_at << "\",\n";
    ss << "  \"exit_code\": " << s.exit_code << ",\n";
    ss << "  \"error_message\": \"" << s.error_message << "\",\n";
    ss << "  \"network\": {\n";
    ss << "    \"ip_address\": \"" << ip << "\",\n";
    ss << "    \"gateway\": \"10.0.100.1\",\n";
    ss << "    \"bridge\": \"kb0\",\n";
    ss << "    \"ports\": [";
    for (size_t i = 0; i < ports.size(); ++i) {
        if (i) ss << ", ";
        ss << "\"" << ports[i] << "\"";
    }
    ss << "]\n";
    ss << "  },\n";
    ss << "  \"cgroups\": {\n";
    ss << "    \"memory_current_bytes\": " << stats.memory_current << ",\n";
    ss << "    \"memory_limit_bytes\": " << stats.memory_limit << ",\n";
    ss << "    \"memory_peak_bytes\": " << stats.memory_peak << ",\n";
    ss << "    \"cpu_usage_usec\": " << stats.cpu_usage_usec << ",\n";
    ss << "    \"cpu_user_usec\": " << stats.cpu_user_usec << ",\n";
    ss << "    \"cpu_system_usec\": " << stats.cpu_system_usec << ",\n";
    ss << "    \"oom_kills\": " << stats.oom_kills << ",\n";
    ss << "    \"oom_count\": " << stats.oom_count << ",\n";
    ss << "    \"pids_count\": " << stats.pids_current << "\n";
    ss << "  },\n";
    ss << "  \"rootfs\": \"" << containers_dir_ + "/" + id + "/rootfs\"\n";
    ss << "}\n";
    return ss.str();
}

bool ContainerRuntime::pause(const std::string& id_or_name) {
    std::string id = resolve_id(id_or_name);
    ContainerState s = load_state(id);
    if (s.status != ContainerStatus::RUNNING) {
        log_error("Container " + id_or_name + " is not running (status: " + status_to_string(s.status) + ")");
        return false;
    }
    cgroups::CgroupManager cm;
    if (!cm.freeze(id, true)) {
        log_error("Failed to freeze container " + id);
        return false;
    }
    s.status = ContainerStatus::PAUSED;
    save_state(s);
    log_info("Container " + id + " paused");
    return true;
}

bool ContainerRuntime::unpause(const std::string& id_or_name) {
    std::string id = resolve_id(id_or_name);
    ContainerState s = load_state(id);
    if (s.status != ContainerStatus::PAUSED) {
        log_error("Container " + id_or_name + " is not paused (status: " + status_to_string(s.status) + ")");
        return false;
    }
    cgroups::CgroupManager cm;
    if (!cm.freeze(id, false)) {
        log_error("Failed to unfreeze container " + id);
        return false;
    }
    s.status = ContainerStatus::RUNNING;
    save_state(s);
    log_info("Container " + id + " unpaused");
    return true;
}

} // namespace kernelbox::runtime
