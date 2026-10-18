// KernelBox — Network Module Implementation
// Bridge, veth pairs, iptables port forwarding

#include "../../include/network/network_manager.hpp"
#include "../../include/common.hpp"

#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <unistd.h>
#include <sys/wait.h>

namespace fs = std::filesystem;

namespace kernelbox::network {

// ── Helpers ──────────────────────────────────────────────────────────────────

bool NetworkManager::run_cmd(const std::string& cmd) {
    log_debug("exec: " + cmd);
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

    // Assign IP (ignore EEXIST)
    run_cmd("ip addr add " + bridge_ip_ + "/24 dev " + bridge_name_ + " 2>/dev/null");
    if (!run_cmd("ip link set " + bridge_name_ + " up")) return false;

    // Enable IP forwarding
    std::ofstream fwd("/proc/sys/net/ipv4/ip_forward");
    if (fwd) { fwd << "1"; fwd.close(); }
    else { log_error("Cannot enable IP forwarding"); return false; }

    // iptables MASQUERADE (idempotent: check first, add if missing)
    run_cmd("iptables -t nat -C POSTROUTING -s " + bridge_subnet_ + " ! -o " + bridge_name_ +
            " -j MASQUERADE 2>/dev/null || "
            "iptables -t nat -A POSTROUTING -s " + bridge_subnet_ + " ! -o " + bridge_name_ +
            " -j MASQUERADE");

    // Masquerade host traffic into bridge so 127.0.0.1 DNAT returns properly
    run_cmd("iptables -t nat -C POSTROUTING -o " + bridge_name_ + " -j MASQUERADE 2>/dev/null || "
            "iptables -t nat -A POSTROUTING -o " + bridge_name_ + " -j MASQUERADE");

    // Allow routing localnet on bridge
    run_cmd("sysctl -w net.ipv4.conf." + bridge_name_ + ".route_localnet=1 >/dev/null 2>&1");

    // FORWARD rules
    run_cmd("iptables -C FORWARD -i " + bridge_name_ + " -j ACCEPT 2>/dev/null || "
            "iptables -A FORWARD -i " + bridge_name_ + " -j ACCEPT");
    run_cmd("iptables -C FORWARD -o " + bridge_name_ + " -j ACCEPT 2>/dev/null || "
            "iptables -A FORWARD -o " + bridge_name_ + " -j ACCEPT");

    log_info("Bridge " + bridge_name_ + " ready");
    return true;
}

// ── veth pair ────────────────────────────────────────────────────────────────

bool NetworkManager::create_veth_pair(const std::string& container_id, pid_t pid) {
    std::string vh = veth_host(container_id);
    std::string vc = veth_cont(container_id);

    log_info("Creating veth pair " + vh + " <-> " + vc);
    if (!run_cmd("ip link add " + vh + " type veth peer name " + vc)) return false;
    if (!run_cmd("ip link set " + vh + " master " + bridge_name_))    return false;
    if (!run_cmd("ip link set " + vh + " up"))                        return false;
    if (!run_cmd("ip link set " + vc + " netns " + std::to_string(pid))) return false;

    return true;
}

// ── Container-side networking ────────────────────────────────────────────────

bool NetworkManager::configure_container_network(const std::string& container_id, pid_t pid) {
    std::string ip = "10.0.100." + std::to_string(next_ip_octet_++);
    std::string vc = veth_cont(container_id);
    std::string ns = "nsenter -t " + std::to_string(pid) + " -n -- ";

    log_info("Configuring container network " + container_id + " → " + ip);

    if (!run_cmd(ns + "ip addr add " + ip + "/24 dev " + vc))     return false;
    if (!run_cmd(ns + "ip link set " + vc + " up"))                return false;
    if (!run_cmd(ns + "ip link set lo up"))                        return false;
    if (!run_cmd(ns + "ip route add default via " + bridge_ip_))   return false;

    // Persist the assigned IP
    std::string conf_dir = CONTAINERS_DIR + "/" + container_id;
    fs::create_directories(conf_dir);
    std::ofstream ofs(conf_dir + "/network.conf");
    if (ofs) ofs << ip << "\n";

    return true;
}

// ── Port mapping ─────────────────────────────────────────────────────────────

bool NetworkManager::add_port_mapping(const std::string& host_addr,
                                       const std::string& host_port,
                                       const std::string& container_ip,
                                       const std::string& container_port) {
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

bool NetworkManager::cleanup(const std::string& container_id) {
    log_info("Cleaning up network for " + container_id);
    run_cmd("ip link delete " + veth_host(container_id) + " 2>/dev/null");
    return true;
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
    std::string path = CONTAINERS_DIR + "/" + container_id + "/network.conf";
    std::ifstream ifs(path);
    std::string ip;
    if (ifs >> ip) return ip;
    return "";
}

} // namespace kernelbox::network
