#!/bin/bash
# Build the ClassiCube webOS IPK and install/launch it on the TV.
set -e
cd "$(dirname "$0")"

# Build + package
make -f Makefile.webos

# Set PATH for ares tools
ARES_DIR=/run/media/1TB/Dev-Projects/Porting/Tools/ares-cli-rs-v0.2.0-linux-x86_64
export PATH=$ARES_DIR:$PATH

# Default device (override with: ARES_DEVICE=<name> ./install.sh)
export ARES_DEVICE=${ARES_DEVICE:-tv-sala}

# Install package onto TV
ares-install webos/com.classicube.webos_${1:-1.0.0}_arm.ipk

# Launch application
ares-launch com.classicube.webos
