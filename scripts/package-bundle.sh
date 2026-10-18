#!/bin/bash
set -e

# KernelBox Bundle Packager
# Packages the runtime executable + containerized backend image into a shareable tarball

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DIST_DIR="$PROJECT_DIR/dist/kernelbox-demo"
BUNDLE_ARCHIVE="$PROJECT_DIR/kernelbox-demo.tar.gz"

echo "=================================================="
echo " Packaging KernelBox Demo for Distribution"
echo "=================================================="

# 1. Clean previous build
rm -rf "$DIST_DIR" "$BUNDLE_ARCHIVE"
mkdir -p "$DIST_DIR"

# 2. Compile the mini binary
echo "[1/4] Compiling KernelBox engine..."
cd "$PROJECT_DIR"
g++ -std=c++17 -Wall -Wextra -Iinclude -pthread src/*.cpp src/*/*.cpp -o mini
cp mini "$DIST_DIR/"

# 3. Export the container image
echo "[2/4] Exporting container image demo-api:v1..."
if [ ! -d "/var/lib/kernelbox/images/demo-api_v1" ] && [ ! -d "/var/lib/portarun/images/demo-api_v1" ]; then
    echo "Image demo-api:v1 not found. Building it first..."
    sudo ./mini build examples/node-api -t demo-api:v1
fi
sudo ./mini export demo-api:v1 -o "$DIST_DIR/demo-api.tar.gz"
sudo chown $(id -u):$(id -g) "$DIST_DIR/demo-api.tar.gz"

# 4. Create the run script for the friend
echo "[3/4] Creating friend run/stop scripts..."
cat << 'EOF' > "$DIST_DIR/run.sh"
#!/bin/bash
set -e

echo "=================================================="
echo " KernelBox: Running Containerized Backend"
echo " (No Node.js or Docker required on this system!)"
echo "=================================================="

# Check root
if [ "$EUID" -ne 0 ]; then
    echo "Please run with sudo: sudo ./run.sh"
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# 1. Import the container image into KernelBox
echo "[1/3] Importing backend container image..."
./mini import demo-api.tar.gz

# 2. Run the container
echo "[2/3] Starting container in background..."
./mini run demo-api:v1 --name friend-api --publish 3000:3000 --memory 128m -d

# 3. Verify it's responding
echo "[3/3] Waiting for container to start..."
sleep 2

echo "Container status:"
./mini ps

echo ""
echo "Testing backend health endpoint..."
if curl -s http://127.0.0.1:3000/health; then
    echo ""
    echo " SUCCESS! The backend is up and running."
elif curl -s http://10.0.100.2:3000/health; then
    echo ""
    echo " SUCCESS! The backend is up and running via bridge IP."
else
    echo "Container started. Check logs: sudo ./mini logs friend-api"
fi

echo ""
echo "Useful commands:"
echo "  Check status:  sudo ./mini ps"
echo "  View stats:    sudo ./mini stats friend-api"
echo "  View logs:     sudo ./mini logs friend-api"
echo "  Stop container: sudo ./stop.sh"
EOF
chmod +x "$DIST_DIR/run.sh"

cat << 'EOF' > "$DIST_DIR/stop.sh"
#!/bin/bash
if [ "$EUID" -ne 0 ]; then
    echo "Please run with sudo: sudo ./stop.sh"
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

echo "Stopping and removing container..."
./mini stop friend-api 2>/dev/null || true
./mini rm friend-api 2>/dev/null || true
echo "Done."
EOF
chmod +x "$DIST_DIR/stop.sh"

# Add README for the friend
cat << 'EOF' > "$DIST_DIR/README.md"
# KernelBox Demo Backend

This folder contains a fully isolated containerized Node.js backend running on our custom Linux container runtime (**KernelBox**).

## Requirements:
- Linux (Ubuntu / Debian or any modern Linux kernel)
- Root / sudo privileges
- **Node.js is NOT required!** (It is packed inside the container image)
- **Docker is NOT required!** (Runs on our custom C++ container engine `mini`)

## How to Run:
```bash
sudo ./run.sh
```

## How to Test:
Open your browser or run:
```bash
curl http://10.0.100.2:3000/health
```

## How to Stop:
```bash
sudo ./stop.sh
```
EOF

# 5. Compress into a single deliverable tarball
echo "[4/4] Creating final archive: kernelbox-demo.tar.gz..."
cd "$PROJECT_DIR/dist"
tar -czf "$BUNDLE_ARCHIVE" kernelbox-demo

echo "=================================================="
echo " Done! Deliverable created at:"
echo " $BUNDLE_ARCHIVE"
echo "=================================================="
echo "You can send 'kernelbox-demo.tar.gz' to your friend!"
