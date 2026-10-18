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
              << "\nCommands:\n"
              << "  build <context_dir> -t <tag>     Build an image from a directory (Boxfile.json/Portafile.json)\n"
              << "  images                           List all images\n"
              << "  run <image> [options]            Create and start a container\n"
              << "    --name <name>                  Container name\n"
              << "    --publish <host:container>     Port mapping (e.g. 8080:80 or 3000:3000)\n"
              << "    --cpu <quota>                  CPU limit (e.g., \"50000 100000\" for 50%)\n"
              << "    --memory <limit>               Memory limit (e.g., 64m, 128m, 1g)\n"
              << "    -d                             Detach (run in background)\n"
              << "  ps                               List containers\n"
              << "  logs <id_or_name>               Show container logs\n"
              << "  stats <id_or_name>              Show container resource usage\n"
              << "  stop <id_or_name>               Stop a container\n"
              << "  rm <id_or_name>                 Remove a container\n"
              << "  export <tag> -o <file>          Export image to file\n"
              << "  import <file>                   Import image from file\n"
              << "  help                            Show help\n";
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
        throw std::invalid_argument("Invalid memory format");
    }
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_help();
        return 1;
    }

    std::string command = argv[1];
    
    if (geteuid() != 0 && command != "help" && command != "images" && command != "ps") {
        log_warn("Most operations require root privileges (sudo).");
    }
    
    runtime::ContainerRuntime runtime;
    image::ImageManager image_manager;
    cgroups::CgroupManager cgroup_manager;

    try {
        if (command == "help") {
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
        } else if (command == "ps") {
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
        } else if (command == "logs") {
            if (argc < 3) { std::cerr << "Usage: mini logs <id_or_name>\n"; return 1; }
            std::cout << runtime.logs(argv[2]);
        } else if (command == "stats") {
            if (argc < 3) { std::cerr << "Usage: mini stats <id_or_name>\n"; return 1; }
            auto state = runtime.get(argv[2]);
            if (!state) { std::cerr << "Container not found\n"; return 1; }
            
            auto stats = cgroup_manager.get_stats(state->id);
            std::cout << "Container: " << state->name << " (" << state->id.substr(0, 12) << ")\n";
            std::cout << "CPU Usage: " << (stats.cpu_usage_usec / 1000) << " ms\n";
            std::cout << "Memory:    " << (stats.memory_current / 1024 / 1024) << " MB";
            if (stats.memory_limit > 0) std::cout << " / " << (stats.memory_limit / 1024 / 1024) << " MB";
            std::cout << "\nProcesses: " << stats.pids_current << "\n";
            
        } else if (command == "stop") {
            if (argc < 3) { std::cerr << "Usage: mini stop <id_or_name>\n"; return 1; }
            runtime.stop(argv[2]);
            std::cout << argv[2] << "\n";
        } else if (command == "rm") {
            if (argc < 3) { std::cerr << "Usage: mini rm <id_or_name>\n"; return 1; }
            runtime.remove(argv[2]);
            std::cout << argv[2] << "\n";
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
