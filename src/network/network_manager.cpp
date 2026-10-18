// KernelBox — Network Module Implementation
// Bridge, veth pairs, iptables port forwarding

#include "../../include/network/network_manager.hpp"
#include "../../include/common.hpp"

#include <arpa/inet.h>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <unistd.h>
#include <sys/wait.h>
#include <cctype>
#include <vector>

namespace fs = std::filesystem;

namespace kernelbox::network {

namespace {

bool valid_container_id(const std::string& id) {
    if (id.size() != 12) return false;
    for (unsigned char c : id) {
        if (!std::isxdigit(c)) return false;
    }
    return true;
}

bool valid_port(const std::string& port) {
    if (port.empty()) return false;
    unsigned int value = 0;
    for (unsigned char c : port) {
        if (!std::isdigit(c)) return false;
        value = value * 10 + (c - '0');
        if (value > 65535) return false;
    }
    return value > 0;
}

bool valid_container_ip(const std::string& ip) {
    constexpr const char* prefix = "10.0.100.";
    if (ip.compare(0, 9, prefix) != 0) return false;
    const std::string octet = ip.substr(9);
    if (!valid_port(octet)) return false;
    const unsigned int value = static_cast<unsigned int>(std::stoul(octet));
    return value >= 2 && value <= 254;
}

bool valid_ipv4(const std::string& ip) {
    in_addr address{};
    return inet_pton(AF_INET, ip.c_str(), &address) == 1;
}

} // namespace

// ── Helpers ──────────────────────────────────────────────────────────────────

bool NetworkManager::run_cmd(const std::string& cmd) {
    log_debug("exec: " + cmd);
    if (command_runner_) return command_runner_(cmd);
    int ret = std::system(cmd.c_str());
    if (ret != 0) {
        log_debug("  → exit " + std::to_string(WEXITSTATUS(ret)));
        return false;
    }
    return true;
}

std::string NetworkManager::veth_host(const std::string& id) {
    return "veth" + id.substr(0, 6) + "h";   // e.g. vethab12cdh  (≤15 chars)
}

std::string NetworkManager::veth_cont(const std::string& id) {
    return "veth" + id.substr(0, 6) + "c";
}

// ── Bridge ───────────────────────────────────────────────────────────────────

bool NetworkManager::setup_bridge() {
    // Idempotent: check if bridge already exists
    if (run_cmd("ip link show " + bridge_name_ + " >/dev/null 2>&1")) {
        log_info("Bridge " + bridge_name_ + " already exists");
    } else {
        log_info("Creating bridge " + bridge_name_);
        if (!run_cmd("ip link add " + bridge_name_ + " type bridge")) return false;
    }

    if (!run_cmd("ip addr replace " + bridge_ip_ + "/24 dev " + bridge_name_)) return false;
    if (!run_cmd("ip link set " + bridge_name_ + " up")) return false;

    // Enable IP forwarding
    std::ofstream fwd("/proc/sys/net/ipv4/ip_forward");
    if (!fwd) {
        log_error("Cannot enable IP forwarding");
        return false;
    }
    fwd << "1";
    fwd.flush();
    if (!fwd) {
        log_error("Failed to enable IP forwarding");
        return false;
    }
    fwd.close();

    // iptables MASQUERADE (idempotent: check first, add if missing)
    if (!run_cmd("iptables -t nat -C POSTROUTING -s " + bridge_subnet_ + " ! -o " + bridge_name_ +
                 " -j MASQUERADE 2>/dev/null || "
                 "iptables -t nat -A POSTROUTING -s " + bridge_subnet_ + " ! -o " + bridge_name_ +
                 " -j MASQUERADE")) return false;

    // Masquerade host traffic into bridge so 127.0.0.1 DNAT returns properly
    if (!run_cmd("iptables -t nat -C POSTROUTING -o " + bridge_name_ + " -j MASQUERADE 2>/dev/null || "
                 "iptables -t nat -A POSTROUTING -o " + bridge_name_ + " -j MASQUERADE")) return false;

    // Allow routing localnet on bridge
    if (!run_cmd("sysctl -w net.ipv4.conf." + bridge_name_ + ".route_localnet=1 >/dev/null 2>&1")) return false;

    // FORWARD rules
    if (!run_cmd("iptables -C FORWARD -i " + bridge_name_ + " -j ACCEPT 2>/dev/null || "
                 "iptables -A FORWARD -i " + bridge_name_ + " -j ACCEPT")) return false;
    if (!run_cmd("iptables -C FORWARD -o " + bridge_name_ + " -j ACCEPT 2>/dev/null || "
                 "iptables -A FORWARD -o " + bridge_name_ + " -j ACCEPT")) return false;

    log_info("Bridge " + bridge_name_ + " ready");
    return true;
}

// ── veth pair ────────────────────────────────────────────────────────────────

bool NetworkManager::create_veth_pair(const std::string& container_id, pid_t pid) {
    if (!valid_container_id(container_id) || pid <= 0) return false;
    std::string vh = veth_host(container_id);
    std::string vc = veth_cont(container_id);

    log_info("Creating veth pair " + vh + " <-> " + vc);
    if (!run_cmd("ip link add " + vh + " type veth peer name " + vc)) return false;
    auto rollback = [&]() {
        if (!run_cmd("ip link delete " + vh + " 2>/dev/null")) {
            log_error("Failed to roll back veth " + vh);
        }
    };
    if (!run_cmd("ip link set " + vh + " master " + bridge_name_)) {
        rollback();
        return false;
    }
    if (!run_cmd("ip link set " + vh + " up")) {
        rollback();
        return false;
    }
    if (!run_cmd("ip link set " + vc + " netns " + std::to_string(pid))) {
        rollback();
        return false;
    }

    return true;
}

// ── Container-side networking ────────────────────────────────────────────────

bool NetworkManager::configure_container_network(const std::string& container_id, pid_t pid) {
    if (!valid_container_id(container_id) || pid <= 0) return false;
    std::string ip = "10.0.100." + std::to_string(next_ip_octet_++);
    std::string vc = veth_cont(container_id);
    std::string ns = "nsenter -t " + std::to_string(pid) + " -n -- ";

    log_info("Configuring container network " + container_id + " → " + ip);

    if (!run_cmd(ns + "ip addr add " + ip + "/24 dev " + vc))     return false;
    if (!run_cmd(ns + "ip link set " + vc + " up"))                return false;
    if (!run_cmd(ns + "ip link set lo up"))                        return false;
    if (!run_cmd(ns + "ip route add default via " + bridge_ip_))   return false;

    // Persist the assigned IP
    std::string conf_dir = containers_dir_ + "/" + container_id;
    std::error_code ec;
    fs::create_directories(conf_dir, ec);
    if (ec) {
        log_error("Create network config directory: " + ec.message());
        return false;
    }
    std::ofstream ofs(conf_dir + "/network.conf");
    if (!ofs) {
        log_error("Open network config for " + container_id);
        return false;
    }
    ofs << ip << "\n";
    ofs.close();
    if (!ofs) {
        log_error("Write network config for " + container_id);
        return false;
    }

    return true;
}

// ── Port mapping ─────────────────────────────────────────────────────────────

bool NetworkManager::add_port_mapping(const std::string& host_addr,
                                       const std::string& host_port,
                                       const std::string& container_ip,
                                       const std::string& container_port) {
    if (!valid_port(host_port) || !valid_port(container_port) ||
        !valid_container_ip(container_ip) ||
        (!host_addr.empty() && host_addr != "0.0.0.0" && !valid_ipv4(host_addr))) {
        log_error("Invalid port mapping");
        return false;
    }
    log_info("Port " + host_addr + ":" + host_port + " → " + container_ip + ":" + container_port);

    std::string dest = container_ip + ":" + container_port;
    std::string d_flag = (host_addr == "0.0.0.0" || host_addr.empty()) ? "" : " -d " + host_addr;

    if (!run_cmd("iptables -t nat -A PREROUTING -p tcp" + d_flag +
                 " --dport " + host_port + " -j DNAT --to-destination " + dest))
        return false;
    if (!run_cmd("iptables -t nat -A OUTPUT -p tcp" + d_flag +
                 " --dport " + host_port + " -j DNAT --to-destination " + dest))
        return false;
    if (!run_cmd("iptables -A FORWARD -p tcp -d " + container_ip +
                 " --dport " + container_port + " -j ACCEPT"))
        return false;
    return true;
}

// ── Cleanup ──────────────────────────────────────────────────────────────────

bool NetworkManager::remove_port_mapping(const std::string& host_port,
                                         const std::string& container_ip,
                                         const std::string& container_port) {
    if (!valid_port(host_port) || !valid_port(container_port) ||
        !valid_container_ip(container_ip)) {
        log_error("Invalid persisted port mapping");
        return false;
    }

    const std::string dest = container_ip + ":" + container_port;
    const std::vector<std::string> rules = {
        "iptables -t nat -D PREROUTING -p tcp --dport " + host_port +
            " -j DNAT --to-destination " + dest,
        "iptables -t nat -D OUTPUT -p tcp --dport " + host_port +
            " -j DNAT --to-destination " + dest,
        "iptables -D FORWARD -p tcp -d " + container_ip +
            " --dport " + container_port + " -j ACCEPT"
    };

    bool success = true;
    for (const auto& rule : rules) {
        std::string check = rule;
        check.replace(check.find(" -D "), 4, " -C ");
        if (run_cmd(check) && !run_cmd(rule)) success = false;
    }
    return success;
}

bool NetworkManager::cleanup(const std::string& container_id) {
    if (!valid_container_id(container_id)) {
        log_error("Invalid container ID for network cleanup: " + container_id);
        return false;
    }
    log_info("Cleaning up network for " + container_id);
    bool success = true;
    const std::string conf_dir = containers_dir_ + "/" + container_id;
    std::ifstream network_conf(conf_dir + "/network.conf");
    std::string ip;
    if (network_conf >> ip && valid_container_ip(ip)) {
        std::ifstream ports_conf(conf_dir + "/ports.conf");
        std::string line;
        while (std::getline(ports_conf, line)) {
            const auto colon = line.find(':');
            if (colon == std::string::npos ||
                !remove_port_mapping(line.substr(0, colon), ip, line.substr(colon + 1))) {
                success = false;
            }
        }
        if (ports_conf.bad()) {
            log_error("Read port mappings for " + container_id);
            success = false;
        }
    } else if (network_conf.bad()) {
        log_error("Read network config for " + container_id);
        success = false;
    }
    if (!run_cmd("ip link delete " + veth_host(container_id) + " 2>/dev/null")) {
        log_warn("Could not delete veth for " + container_id + " (it may already be absent)");
    }
    std::error_code ec;
    fs::remove(conf_dir + "/network.conf", ec);
    if (ec) {
        log_error("Remove network config for " + container_id + ": " + ec.message());
        success = false;
    }
    return success;
}

bool NetworkManager::cleanup_bridge() {
    log_info("Cleaning up bridge " + bridge_name_);
    run_cmd("ip link set " + bridge_name_ + " down 2>/dev/null");
    run_cmd("ip link delete " + bridge_name_ + " 2>/dev/null");
    run_cmd("iptables -t nat -D POSTROUTING -s " + bridge_subnet_ + " ! -o " + bridge_name_ + " -j MASQUERADE 2>/dev/null");
    run_cmd("iptables -D FORWARD -i " + bridge_name_ + " -j ACCEPT 2>/dev/null");
    run_cmd("iptables -D FORWARD -o " + bridge_name_ + " -j ACCEPT 2>/dev/null");

    // Also clean up legacy pr0 bridge if present
    run_cmd("ip link delete pr0 2>/dev/null || true");
    return true;
}

std::string NetworkManager::get_container_ip(const std::string& container_id) {
    if (!valid_container_id(container_id)) return "";
    std::string path = containers_dir_ + "/" + container_id + "/network.conf";
    std::ifstream ifs(path);
    std::string ip;
    if (ifs >> ip) return ip;
    return "";
}

} // namespace kernelbox::network
