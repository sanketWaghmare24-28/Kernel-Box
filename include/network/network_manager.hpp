// KernelBox — Network Module
// Manages virtual Ethernet pairs, bridge, and port forwarding for containers

#pragma once
#include <functional>
#include <string>
#include <sys/types.h>
#include <utility>
#include "../common.hpp"

namespace kernelbox::network {

class NetworkManager {
public:
    explicit NetworkManager(
        std::function<bool(const std::string&)> command_runner = {},
        std::string containers_dir = CONTAINERS_DIR)
        : command_runner_(std::move(command_runner)),
          containers_dir_(std::move(containers_dir)) {}

    /// Create the host bridge (kb0) with IP 10.0.100.1/24, enable forwarding.
    bool setup_bridge();

    /// Create a veth pair and attach one end to the bridge, move other into container ns.
    bool create_veth_pair(const std::string& container_id, pid_t pid);

    /// Configure IP, loopback, and default route inside the container's network namespace.
    bool configure_container_network(const std::string& container_id, pid_t pid);

    /// Add iptables DNAT + FORWARD rules for a port mapping.
    bool add_port_mapping(const std::string& host_addr, const std::string& host_port,
                          const std::string& container_ip, const std::string& container_port);

    /// Delete the host veth (auto-deletes container veth too).
    bool cleanup(const std::string& container_id);

    /// Tear down bridge and remove iptables rules.
    bool cleanup_bridge();

    /// Return the container's assigned IP (read from network.conf).
    std::string get_container_ip(const std::string& container_id);

    /// Defined IP address pool constants (10.0.100.2 - 10.0.100.254)
    static constexpr int IP_POOL_START = 2;
    static constexpr int IP_POOL_END   = 254;

    /// Allocate a unique, unused IP address from the pool (persisted atomically).
    std::string allocate_ip(const std::string& container_id);

    /// Release the IP allocated to the container.
    bool release_ip(const std::string& container_id);

private:
    std::string bridge_name_   = "kb0";
    std::string bridge_ip_     = "10.0.100.1";
    std::string bridge_subnet_ = "10.0.100.0/24";

    std::function<bool(const std::string&)> command_runner_;
    std::string containers_dir_;

    bool        run_cmd(const std::string& cmd);
    std::string veth_host(const std::string& container_id);
    std::string veth_cont(const std::string& container_id);
    bool remove_port_mapping(const std::string& host_port,
                             const std::string& container_ip,
                             const std::string& container_port);
};

} // namespace kernelbox::network
