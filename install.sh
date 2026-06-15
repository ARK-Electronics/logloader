#!/bin/bash
set -euo pipefail

THIS_DIR="$(dirname "$(realpath "$BASH_SOURCE")")"

# Build
pushd "$THIS_DIR" > /dev/null
make
popd > /dev/null

# Install binary + pymavlink download backend
sudo mkdir -p /opt/ark/bin
sudo cp "$THIS_DIR/build/logloader" /opt/ark/bin/
sudo cp "$THIS_DIR/logloader_download.py" /opt/ark/bin/
sudo chmod +x /opt/ark/bin/logloader_download.py

# Install default config
sudo mkdir -p /opt/ark/share/logloader
sudo cp "$THIS_DIR/config.toml" /opt/ark/share/logloader/

# Create data directory
mkdir -p "${XDG_DATA_HOME:-$HOME/.local/share}/ark/logloader/logs"
