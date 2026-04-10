#!/bin/bash
# One-time setup for Exodia MP server on a fresh EC2 instance (Amazon Linux 2023 / Ubuntu)
set -euo pipefail

echo "=== Exodia MP Server Setup ==="

# Detect package manager
if command -v dnf &>/dev/null; then
    PKG="dnf"
    sudo dnf install -y gcc-c++ make nginx
elif command -v apt-get &>/dev/null; then
    PKG="apt"
    sudo apt-get update
    sudo apt-get install -y g++ make nginx
else
    echo "Unsupported package manager"; exit 1
fi

# Create exodia user
if ! id exodia &>/dev/null; then
    sudo useradd --system --home-dir /opt/exodia --shell /bin/false exodia
    echo "Created exodia user"
fi

# Create directory structure
sudo mkdir -p /opt/exodia/{bin,worlds}
sudo chown -R exodia:exodia /opt/exodia

# Build the server (assumes we're in the repo root)
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(dirname "$SCRIPT_DIR")"

echo "Building server..."
g++ -std=c++17 -O2 -DNDEBUG -o /tmp/exodia-server "$REPO_DIR/src/server/main.cpp" -I"$REPO_DIR/src"
sudo cp /tmp/exodia-server /opt/exodia/bin/server
sudo chown exodia:exodia /opt/exodia/bin/server
rm /tmp/exodia-server
echo "Server binary installed"

# Install management script
sudo cp "$SCRIPT_DIR/exodia-ctl" /opt/exodia/bin/exodia-ctl
sudo chmod +x /opt/exodia/bin/exodia-ctl
sudo ln -sf /opt/exodia/bin/exodia-ctl /usr/local/bin/exodia-ctl
echo "Management script installed"

# Install systemd service template
sudo cp "$SCRIPT_DIR/exodia-world@.service" /etc/systemd/system/
sudo systemctl daemon-reload
echo "Systemd service template installed"

# Configure nginx
sudo cp "$SCRIPT_DIR/nginx-exodia.conf" /etc/nginx/conf.d/exodia.conf
# Remove default site if it exists (Ubuntu)
sudo rm -f /etc/nginx/sites-enabled/default 2>/dev/null || true
sudo systemctl enable nginx
sudo systemctl restart nginx
echo "Nginx configured"

# Initialize empty lobby
echo '{"host":"'$(curl -s --max-time 2 http://169.254.169.254/latest/meta-data/public-ipv4 2>/dev/null || echo "0.0.0.0")'","worlds":[]}' | sudo tee /opt/exodia/lobby.json > /dev/null
sudo chown exodia:exodia /opt/exodia/lobby.json

echo ""
echo "=== Setup Complete ==="
echo ""
echo "Next steps:"
echo "  sudo exodia-ctl create myworld --desc \"My first world\""
echo "  sudo exodia-ctl start myworld"
echo "  sudo exodia-ctl list"
echo ""
echo "Connect from client:"
echo "  ./client --host <this-ip> --port 10000"
