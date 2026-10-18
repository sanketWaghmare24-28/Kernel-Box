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
