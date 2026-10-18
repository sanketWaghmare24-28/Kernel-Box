#include "../include/process/process_manager.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <unistd.h>

namespace fs = std::filesystem;

int main() {
    const fs::path temp = fs::temp_directory_path() /
        ("kernelbox-process-test-" + std::to_string(getpid()));
    fs::create_directories(temp);
    const fs::path blocked_marker = temp / "must-not-run";
    const fs::path attached_marker = temp / "attached";
    kernelbox::process::ProcessManager manager;

    pid_t blocked = manager.spawn(
        {"/usr/bin/touch", blocked_marker.string()}, {}, "", "", 0, -1, -1,
        [] { return false; },
        [](pid_t) { return true; });
    if (blocked != -1 || fs::exists(blocked_marker)) {
        std::cerr << "Child executed despite failed filesystem setup\n";
        fs::remove_all(temp);
        return 1;
    }

    pid_t attached = manager.spawn(
        {"/usr/bin/test", "-f", attached_marker.string()}, {}, "", "", 0, -1, -1,
        [] { return true; },
        [&attached_marker](pid_t) {
            std::ofstream(attached_marker) << "attached\n";
            return true;
        });
    if (attached <= 0 || manager.wait_for(attached) != 0) {
        std::cerr << "Child did not observe successful parent-side attachment setup\n";
        fs::remove_all(temp);
        return 1;
    }

    const fs::path rejected_marker = temp / "parent-setup-failed";
    pid_t rejected = manager.spawn(
        {"/usr/bin/touch", rejected_marker.string()}, {}, "", "", 0, -1, -1,
        [] { return true; },
        [](pid_t) { return false; });
    if (rejected != -1 || fs::exists(rejected_marker)) {
        std::cerr << "Child executed despite failed parent-side attachment\n";
        fs::remove_all(temp);
        return 1;
    }

    fs::remove_all(temp);
    return 0;
}
