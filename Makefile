CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -Iinclude -pthread
LDFLAGS = -pthread

SRCS = src/main.cpp \
       src/runtime/container.cpp \
       src/image/image_manager.cpp \
       src/cgroups/cgroup_manager.cpp \
       src/network/network_manager.cpp \
       src/process/process_manager.cpp \
       src/filesystem/rootfs.cpp \
       src/logging/log_manager.cpp

OBJS = $(SRCS:.cpp=.o)

TARGET = mini

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

test:
	sh tests/run-linux-tests.sh
	cd dashboard && npm test

test-networking:
	sudo bash tests/test_scenario_e_networking.sh

test-lifecycle:
	sudo bash tests/test_scenario_g_lifecycle.sh

test-cache:
	sudo bash tests/test_scenario_f_cache.sh

.PHONY: all clean test test-networking test-lifecycle test-cache
