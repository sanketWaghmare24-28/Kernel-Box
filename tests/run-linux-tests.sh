#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=$(mktemp -d)
trap 'rm -rf "$build_dir"' EXIT

cxx=${CXX:-g++}

"$cxx" -std=c++17 -Wall -Wextra -I"$repo_root/include" \
    "$repo_root/tests/image_manager_test.cpp" "$repo_root/src/image/image_manager.cpp" \
    -o "$build_dir/image_manager_test"
"$cxx" -std=c++17 -Wall -Wextra -I"$repo_root/include" \
    "$repo_root/tests/process_manager_test.cpp" "$repo_root/src/process/process_manager.cpp" \
    -o "$build_dir/process_manager_test"
"$cxx" -std=c++17 -Wall -Wextra -I"$repo_root/include" \
    "$repo_root/tests/cgroup_manager_test.cpp" "$repo_root/src/cgroups/cgroup_manager.cpp" \
    -o "$build_dir/cgroup_manager_test"
"$cxx" -std=c++17 -Wall -Wextra -I"$repo_root/include" \
    "$repo_root/tests/network_manager_test.cpp" "$repo_root/src/network/network_manager.cpp" \
    "$repo_root/src/process/process_manager.cpp" \
    -o "$build_dir/network_manager_test"

"$build_dir/image_manager_test"
"$build_dir/process_manager_test"
"$build_dir/cgroup_manager_test"
"$build_dir/network_manager_test"
