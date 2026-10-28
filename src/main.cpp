// KernelBox — CLI Entry Point

#include "../include/runtime/container.hpp"
#include "../include/image/image_manager.hpp"
#include "../include/cgroups/cgroup_manager.hpp"
#include "../include/process/process_manager.hpp"
#include "../include/common.hpp"

#include <iostream>
#include <vector>
#include <string>
#include <iomanip>
#include <stdexcept>
#include <unistd.h>

using namespace kernelbox;

void print_help() {
    std::cout << "KernelBox - Lightweight Linux Container Runtime\n"
              << "Usage: mini <command> [options]\n"
              << "\nLifecycle Commands:\n"
              << "  create <image> [options]         Create container rootfs and metadata without starting\n"
              << "  start <id_or_name>               Start a created or stopped container\n"
              << "  run <image> [options]            Create and immediately start a container\n"
              << "    --name <name>                  Container name\n"
              << "    --publish <host:container>     Port mapping (e.g. 8080:80 or 3000:3000)\n"
              << "    --cpu <quota>                  CPU limit (e.g. \"50000 100000\" for 50%)\n"
              << "    --memory <limit>               Memory limit (e.g. 64m, 128m, 1g)\n"
              << "    -d                             Detach (run in background)\n"
              << "  ps / list                        List all containers and their status\n"
              << "  stop <id_or_name>                Stop a running container\n"
              << "  restart <id_or_name>             Restart a container\n"
              << "  rm / remove <id_or_name> [-f]    Remove a container (use -f/--force if running)\n"
              << "\nExecution & Inspection Commands:\n"
              << "  exec <id_or_name> <cmd...>       Execute a command or shell inside a running container\n"
              << "  inspect <id_or_name>             Display detailed container JSON metadata\n"
              << "  pause <id_or_name>               Freeze/pause all processes in a container\n"
              << "  unpause <id_or_name>             Unfreeze/resume all processes in a container\n"
              << "  logs <id_or_name>                Show container logs\n"
              << "  stats <id_or_name>               Show container cgroup resource usage & OOM stats\n"
              << "\nImage Management Commands:\n"
              << "  build <context_dir> -t <tag>     Build an image from a directory (Boxfile.json/Portafile.json)\n"
              << "  images                           List all local images\n"
              << "  export <tag> -o <file>           Export image to tarball\n"
              << "  import <file>                    Import image from tarball\n"
              << "  help                             Show this help message\n";
}

std::string parse_memory(const std::string& mem) {
    if (mem.empty()) return "";
    size_t pos = 0;
    try {
        uint64_t val = std::stoull(mem, &pos);
        if (pos < mem.length()) {
            char unit = mem[pos];
            if (unit == 'm' || unit == 'M') val *= 1024 * 1024;
            else if (unit == 'g' || unit == 'G') val *= 1024 * 1024 * 1024;
            else if (unit == 'k' || unit == 'K') val *= 1024;
        }
        return std::to_string(val);
    } catch (...) {
        throw std::invalid_argument("Invalid memory format: " + mem);
    }
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_help();
        return 1;
    }

    std::string command = argv[1];

    if (geteuid() != 0 && command != "help" && command != "images" && command != "ps" && command != "list") {
        log_warn("Most operations require root privileges (sudo).");
    }

    runtime::ContainerRuntime runtime;
    image::ImageManager image_manager;
    cgroups::CgroupManager cgroup_manager;

    try {
        if (command == "help" || command == "--help" || command == "-h") {
            print_help();
        } else if (command == "build") {
            if (argc < 5 || std::string(argv[3]) != "-t") {
                std::cerr << "Usage: mini build <context_dir> -t <tag>\n";
                return 1;
            }
            if (!image_manager.build(argv[2], argv[4])) {
                std::cerr << "Build failed.\n";
                return 1;
            }
        } else if (command == "images") {
            auto images = image_manager.list();
            std::cout << std::left << std::setw(20) << "REPOSITORY"
                      << std::setw(20) << "TAG"
                      << std::setw(15) << "SIZE (KB)" << "\n";
            for (const auto& img : images) {
                std::cout << std::left << std::setw(20) << img.name
                          << std::setw(20) << img.version
                          << std::setw(15) << (img.size_bytes / 1024) << "\n";
            }
        } else if (command == "create") {
            if (argc < 3) {
                std::cerr << "Usage: mini create <image> [options]\n";
                return 1;
            }
            std::string image = argv[2];
            std::string name;
            std::string cpu_limit, memory_limit;
            std::vector<std::pair<std::string,std::string>> ports;

            for (int i = 3; i < argc; ++i) {
                std::string arg = argv[i];
                if (arg == "--name" && i + 1 < argc) name = argv[++i];
                else if (arg == "--cpu" && i + 1 < argc) cpu_limit = argv[++i];
                else if (arg == "--memory" && i + 1 < argc) memory_limit = argv[++i];
                else if (arg == "--publish" && i + 1 < argc) {
                    std::string p = argv[++i];
                    auto pos = p.find(':');
                    if (pos != std::string::npos) {
                        ports.push_back({p.substr(0, pos), p.substr(pos + 1)});
                    }
                }
            }

            if (name.empty()) name = "container_" + generate_id().substr(0, 6);
            std::string parsed_mem = parse_memory(memory_limit);
            std::string id = runtime.create(image, name, ports, cpu_limit, parsed_mem, false);
            std::cout << id << "\n";
        } else if (command == "start") {
            if (argc < 3) {
                std::cerr << "Usage: mini start <id_or_name>\n";
                return 1;
            }
            if (!runtime.start(argv[2])) return 1;
            std::cout << argv[2] << "\n";
        } else if (command == "run") {
            if (argc < 3) {
                std::cerr << "Usage: mini run <image> [options]\n";
                return 1;
            }
            std::string image = argv[2];
            std::string name;
            std::string cpu_limit, memory_limit;
            bool detach = false;
            std::vector<std::pair<std::string,std::string>> ports;

            for (int i = 3; i < argc; ++i) {
                std::string arg = argv[i];
                if (arg == "--name" && i + 1 < argc) name = argv[++i];
                else if (arg == "--cpu" && i + 1 < argc) cpu_limit = argv[++i];
                else if (arg == "--memory" && i + 1 < argc) memory_limit = argv[++i];
                else if (arg == "-d") detach = true;
                else if (arg == "--publish" && i + 1 < argc) {
                    std::string p = argv[++i];
                    auto pos = p.find(':');
                    if (pos != std::string::npos) {
                        ports.push_back({p.substr(0, pos), p.substr(pos + 1)});
                    }
                }
            }

            if (name.empty()) name = "container_" + generate_id().substr(0, 6);

            std::string parsed_mem = parse_memory(memory_limit);
            std::string id = runtime.create(image, name, ports, cpu_limit, parsed_mem, detach);
            runtime.start(id);

            if (!detach) {
                // In foreground mode, wait for the container to exit
                auto state = runtime.get(id);
                if (state && state->pid > 0) {
                    process::ProcessManager pm;
                    int exit_code = pm.wait_for(state->pid);
                    runtime.stop(id); // Ensure cleanup
                    return exit_code;
                }
            } else {
                std::cout << id << "\n";
            }
        } else if (command == "ps" || command == "list") {
            auto states = runtime.list();
            std::cout << std::left << std::setw(15) << "CONTAINER ID"
                      << std::setw(20) << "IMAGE"
                      << std::setw(20) << "STATUS"
                      << "NAMES\n";
            for (const auto& s : states) {
                std::cout << std::left << std::setw(15) << s.id.substr(0, 12)
                          << std::setw(20) << s.image
                          << std::setw(20) << status_to_string(s.status)
                          << s.name << "\n";
            }
        } else if (command == "exec") {
            if (argc < 4) {
                std::cerr << "Usage: mini exec <id_or_name> <command> [args...]\n";
                return 1;
            }
            std::vector<std::string> exec_cmd;
            for (int i = 3; i < argc; ++i) exec_cmd.push_back(argv[i]);
            return runtime.exec(argv[2], exec_cmd);
        } else if (command == "inspect") {
            if (argc < 3) { std::cerr << "Usage: mini inspect <id_or_name>\n"; return 1; }
            std::cout << runtime.inspect(argv[2]);
        } else if (command == "pause") {
            if (argc < 3) { std::cerr << "Usage: mini pause <id_or_name>\n"; return 1; }
            if (!runtime.pause(argv[2])) return 1;
            std::cout << argv[2] << " paused\n";
        } else if (command == "unpause") {
            if (argc < 3) { std::cerr << "Usage: mini unpause <id_or_name>\n"; return 1; }
            if (!runtime.unpause(argv[2])) return 1;
            std::cout << argv[2] << " unpaused\n";
        } else if (command == "logs") {
            if (argc < 3) { std::cerr << "Usage: mini logs <id_or_name>\n"; return 1; }
            std::cout << runtime.logs(argv[2]);
        } else if (command == "stats") {
            if (argc < 3) { std::cerr << "Usage: mini stats <id_or_name>\n"; return 1; }
            auto state = runtime.get(argv[2]);
            if (!state) { std::cerr << "Container not found: " << argv[2] << "\n"; return 1; }

            auto stats = cgroup_manager.get_stats(state->id);
            std::cout << "Container: " << state->name << " (" << state->id.substr(0, 12) << ")\n";
            std::cout << "CPU Usage: " << (stats.cpu_usage_usec / 1000) << " ms";
            if (stats.cpu_user_usec > 0 || stats.cpu_system_usec > 0) {
                std::cout << " (user: " << (stats.cpu_user_usec / 1000) << " ms, sys: "
                          << (stats.cpu_system_usec / 1000) << " ms)";
            }
            std::cout << "\nMemory:    " << (stats.memory_current / 1024 / 1024) << " MB";
            if (stats.memory_limit > 0) std::cout << " / " << (stats.memory_limit / 1024 / 1024) << " MB";
            if (stats.memory_peak > 0) std::cout << " (peak: " << (stats.memory_peak / 1024 / 1024) << " MB)";
            std::cout << "\nOOM Kills: " << stats.oom_kills << " (events: " << stats.oom_count << ")\n";
            std::cout << "Processes: " << stats.pids_current << "\n";
        } else if (command == "stop") {
            if (argc < 3) { std::cerr << "Usage: mini stop <id_or_name>\n"; return 1; }
            runtime.stop(argv[2]);
            std::cout << argv[2] << "\n";
        } else if (command == "restart") {
            if (argc < 3) { std::cerr << "Usage: mini restart <id_or_name>\n"; return 1; }
            runtime.restart(argv[2]);
            std::cout << argv[2] << "\n";
        } else if (command == "rm" || command == "remove") {
            if (argc < 3) { std::cerr << "Usage: mini " << command << " <id_or_name> [-f|--force]\n"; return 1; }
            bool force = false;
            std::string target = argv[2];
            for (int i = 2; i < argc; ++i) {
                std::string arg = argv[i];
                if (arg == "-f" || arg == "--force") force = true;
                else target = arg;
            }
            runtime.remove(target, force);
            std::cout << target << "\n";
        } else if (command == "export") {
            if (argc < 5 || std::string(argv[3]) != "-o") { std::cerr << "Usage: mini export <tag> -o <file>\n"; return 1; }
            if (image_manager.export_image(argv[2], argv[4])) std::cout << "Exported to " << argv[4] << "\n";
            else std::cerr << "Export failed\n";
        } else if (command == "import") {
            if (argc < 3) { std::cerr << "Usage: mini import <file>\n"; return 1; }
            if (image_manager.import_image(argv[2])) std::cout << "Import successful\n";
            else std::cerr << "Import failed\n";
        } else {
            std::cerr << "Unknown command: " << command << "\n";
            print_help();
            return 1;
        }
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
