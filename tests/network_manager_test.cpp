#include "../include/network/network_manager.hpp"
#include "../include/process/process_manager.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

int main() {
    const std::string id = "abcdef012345";
    const fs::path temp = fs::temp_directory_path() /
        ("kernelbox-network-test-" + std::to_string(getpid()));
    fs::create_directories(temp / id);

    std::vector<std::string> commands;
    kernelbox::network::NetworkManager bridge_manager(
        [&commands](const std::string& command) {
            commands.push_back(command);
            return command.find("ip link set kb0 up") == std::string::npos;
        },
        temp.string());
    if (bridge_manager.setup_bridge()) {
        std::cerr << "Bridge setup failure was not propagated\n";
        fs::remove_all(temp);
        return 1;
    }

    commands.clear();
    kernelbox::network::NetworkManager veth_manager(
        [&commands](const std::string& command) {
            commands.push_back(command);
            return command.find("master kb0") == std::string::npos;
        },
        temp.string());
    if (veth_manager.create_veth_pair(id, 1234)) {
        std::cerr << "Partial veth setup failure was not propagated\n";
        fs::remove_all(temp);
        return 1;
    }
    if (commands.size() != 3 ||
        commands.back().find("ip link delete vethabcdefh") == std::string::npos) {
        std::cerr << "Partial veth setup was not rolled back\n";
        fs::remove_all(temp);
        return 1;
    }

    commands.clear();
    kernelbox::network::NetworkManager configuration_manager(
        [&commands](const std::string& command) {
            commands.push_back(command);
            return command.find("ip route add default") == std::string::npos;
        },
        temp.string());
    if (configuration_manager.configure_container_network(id, 1234)) {
        std::cerr << "Container network configuration failure was not propagated\n";
        fs::remove_all(temp);
        return 1;
    }
    if (fs::exists(temp / id / "network.conf")) {
        std::cerr << "Failed network configuration did not roll back network.conf\n";
        fs::remove_all(temp);
        return 1;
    }

    // IP Pool Allocation, Reuse, and Exhaustion Regression Tests
    kernelbox::network::NetworkManager pool_manager(
        [](const std::string&) { return true; }, temp.string());

    const std::string id1 = "111111111111";
    const std::string id2 = "222222222222";
    const std::string id3 = "333333333333";
    fs::create_directories(temp / id1);
    fs::create_directories(temp / id2);
    fs::create_directories(temp / id3);

    std::string ip1 = pool_manager.allocate_ip(id1);
    std::string ip2 = pool_manager.allocate_ip(id2);
    std::string ip3 = pool_manager.allocate_ip(id3);

    if (ip1 != "10.0.100.2" || ip2 != "10.0.100.3" || ip3 != "10.0.100.4") {
        std::cerr << "Sequential IP allocation failed: " << ip1 << ", " << ip2 << ", " << ip3 << "\n";
        fs::remove_all(temp);
        return 1;
    }

    // Test release and address reuse
    if (!pool_manager.release_ip(id2)) {
        std::cerr << "Failed to release IP for container 2\n";
        fs::remove_all(temp);
        return 1;
    }
    const std::string id4 = "444444444444";
    fs::create_directories(temp / id4);
    std::string ip4 = pool_manager.allocate_ip(id4);
    if (ip4 != "10.0.100.3") {
        std::cerr << "IP address reuse failed: expected 10.0.100.3, got " << ip4 << "\n";
        fs::remove_all(temp);
        return 1;
    }

    // Test pool exhaustion by creating dummy allocations for the rest of the pool
    for (int octet = 5; octet <= 254; ++octet) {
        char dummy_id[13];
        std::snprintf(dummy_id, sizeof(dummy_id), "d%011d", octet);
        fs::create_directories(temp / dummy_id);
        std::ofstream(temp / dummy_id / "network.conf") << "10.0.100." << octet << "\n";
    }
    const std::string id_overflow = "ffffffffffff";
    fs::create_directories(temp / id_overflow);
    std::string ip_overflow = pool_manager.allocate_ip(id_overflow);
    if (!ip_overflow.empty()) {
        std::cerr << "Pool exhaustion failed to reject allocation: got " << ip_overflow << "\n";
        fs::remove_all(temp);
        return 1;
    }

    std::ofstream(temp / id / "network.conf") << "10.0.100.2\n";
    std::ofstream(temp / id / "ports.conf") << "3000:3000\n";
    commands.clear();
    kernelbox::network::NetworkManager cleanup_manager(
        [&commands](const std::string& command) {
            commands.push_back(command);
            return true;
        },
        temp.string());
    if (!cleanup_manager.cleanup(id)) {
        std::cerr << "Network cleanup failed\n";
        fs::remove_all(temp);
        return 1;
    }
    bool removed_prerouting = false;
    bool removed_output = false;
    bool removed_forward = false;
    for (const auto& command : commands) {
        removed_prerouting |= command.find("iptables -t nat -D PREROUTING") != std::string::npos;
        removed_output |= command.find("iptables -t nat -D OUTPUT") != std::string::npos;
        removed_forward |= command.find("iptables -D FORWARD") != std::string::npos;
    }
    if (!removed_prerouting || !removed_output || !removed_forward ||
        fs::exists(temp / id / "network.conf")) {
        std::cerr << "Network cleanup left port rules or configuration behind\n";
        fs::remove_all(temp);
        return 1;
    }

    const fs::path marker = temp / "network-failure-workload-ran";
    kernelbox::network::NetworkManager failed_network(
        [](const std::string&) { return false; }, temp.string());
    kernelbox::process::ProcessManager process_manager;
    pid_t pid = process_manager.spawn(
        {"/usr/bin/touch", marker.string()}, {}, "", "", 0, -1, -1,
        [] { return true; },
        [&failed_network, &id](pid_t child_pid) {
            return failed_network.create_veth_pair(id, child_pid);
        });
    if (pid != -1 || fs::exists(marker)) {
        std::cerr << "Workload ran despite failed network setup\n";
        fs::remove_all(temp);
        return 1;
    }

    fs::remove_all(temp);
    return 0;
}
