// KernelBox — Logging Module Implementation

#include "../../include/logging/log_manager.hpp"
#include "../../include/common.hpp"

#include <fstream>
#include <sstream>
#include <filesystem>
#include <chrono>
#include <iomanip>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

namespace kernelbox::logging {

bool LogManager::init(const std::string& container_id) {
    fs::path log_dir = fs::path(CONTAINERS_DIR) / container_id / "logs";
    std::error_code ec;
    fs::create_directories(log_dir, ec);
    std::ofstream ofs(log_dir / "container.log", std::ios::app);
    return ofs.good();
}

void LogManager::write(const std::string& container_id, const std::string& stream,
                        const std::string& line) {
    std::string path = get_log_path(container_id);
    int fd = open(path.c_str(), O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (fd < 0) return;

    // Exclusive lock for thread safety
    flock(fd, LOCK_EX);

    auto now = std::chrono::system_clock::now();
    auto t   = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << "[" << std::put_time(std::gmtime(&t), "%Y-%m-%dT%H:%M:%SZ")
       << "] [" << stream << "] " << line << "\n";
    std::string entry = ss.str();

    [[maybe_unused]] auto written = ::write(fd, entry.c_str(), entry.size());

    flock(fd, LOCK_UN);
    close(fd);
}

std::string LogManager::read(const std::string& container_id, int tail_lines) {
    std::ifstream ifs(get_log_path(container_id));
    if (!ifs) return "";

    if (tail_lines <= 0) {
        std::stringstream buf;
        buf << ifs.rdbuf();
        return buf.str();
    }

    std::vector<std::string> lines;
    std::string line;
    while (std::getline(ifs, line)) {
        lines.push_back(line);
        if (static_cast<int>(lines.size()) > tail_lines)
            lines.erase(lines.begin());
    }
    std::stringstream out;
    for (const auto& l : lines) out << l << "\n";
    return out.str();
}

bool LogManager::cleanup(const std::string& container_id) {
    std::error_code ec;
    fs::remove_all(fs::path(CONTAINERS_DIR) / container_id / "logs", ec);
    return !ec;
}

std::string LogManager::get_log_path(const std::string& container_id) {
    return (fs::path(CONTAINERS_DIR) / container_id / "logs" / "container.log").string();
}

} // namespace kernelbox::logging
