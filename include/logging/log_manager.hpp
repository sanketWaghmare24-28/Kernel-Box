// KernelBox — Logging Module
// Container stdout/stderr log capture and retrieval

#pragma once
#include <string>

namespace kernelbox::logging {

class LogManager {
public:
    /// Create the log directory and empty log file.
    bool init(const std::string& container_id);

    /// Append a timestamped line (thread-safe with flock).
    void write(const std::string& container_id, const std::string& stream,
               const std::string& line);

    /// Read all or last N lines of the container log.
    std::string read(const std::string& container_id, int tail_lines = -1);

    /// Remove log directory.
    bool cleanup(const std::string& container_id);

    /// Return the full path to the container's log file.
    std::string get_log_path(const std::string& container_id);
};

} // namespace kernelbox::logging
