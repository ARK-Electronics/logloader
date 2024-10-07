#!/bin/bash

# Setup XDG default paths
DEFAULT_XDG_CONF_HOME="$HOME/.config"
DEFAULT_XDG_DATA_HOME="$HOME/.local/share"
export XDG_CONFIG_HOME="${XDG_CONFIG_HOME:-$DEFAULT_XDG_CONF_HOME}"
export XDG_DATA_HOME="${XDG_DATA_HOME:-$DEFAULT_XDG_DATA_HOME}"
THIS_DIR="$(dirname "$(realpath "$BASH_SOURCE")")"

# Make sure pgk config can find openssl
if ! pkg-config --exists openssl || [[ "$(pkg-config --modversion openssl)" < "3.0.2" ]]; then
	echo "Installing OpenSSL from source"
	$THIS_DIR/install_openssl.sh
fi

# Build the project
pushd .
cd "$THIS_DIR"
make
popd

# Setup project directory
cp $THIS_DIR/build/logloader ~/.local/bin
mkdir -p $XDG_DATA_HOME/logloader/logs
cp $THIS_DIR/config.toml $XDG_DATA_HOME/logloader/

# Modify config file if ENV variables are set
CONFIG_FILE="$XDG_DATA_HOME/logloader/config.toml"

if [ -n "$USER_EMAIL" ]; then
	echo "Setting email to: $USER_EMAIL"
	sed -i "s|^email = \".*\"|email = \"$USER_EMAIL\"|" "$CONFIG_FILE"
fi

if [ -n "$UPLOAD_TO_FLIGHT_REVIEW" ]; then
	if [ "$UPLOAD_TO_FLIGHT_REVIEW" = "y" ]; then
		echo "Log upload enabled"
		sed -i "s|^upload_enabled = .*|upload_enabled = true|" "$CONFIG_FILE"
	else
		echo "Log upload disabled"
		sed -i "s|^upload_enabled = .*|upload_enabled = false|" "$CONFIG_FILE"
	fi
fi

if [ -n "$PUBLIC_LOGS" ]; then
	if [ "$PUBLIC_LOGS" = "y" ]; then
		echo "Public logs enabled"
		sed -i "s|^public_logs = .*|public_logs = true|" "$CONFIG_FILE"
	else
		echo "Public logs disabled"
		sed -i "s|^public_logs = .*|public_logs = false|" "$CONFIG_FILE"
	fi
fi
