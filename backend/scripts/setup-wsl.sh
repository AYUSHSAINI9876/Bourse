#!/usr/bin/env bash
#
# One-shot development environment provisioning for Ubuntu / WSL.
#
#   wsl -d Ubuntu --user root -- bash scripts/setup-wsl.sh
#
# Installs the compiler, build system, test framework and the client tools used
# by the demo and benchmark flows (redis-cli, curl, ab).
set -euo pipefail

export DEBIAN_FRONTEND=noninteractive

echo "==> apt-get update"
apt-get update -y -qq

echo "==> installing toolchain"
apt-get install -y -qq --no-install-recommends \
  build-essential \
  g++ \
  cmake \
  ninja-build \
  git \
  pkg-config \
  ca-certificates \
  libgtest-dev \
  redis-tools \
  curl \
  netcat-openbsd \
  apache2-utils \
  jq

echo
echo "==> versions"
g++ --version | head -n 1
cmake --version | head -n 1
ninja --version | sed 's/^/ninja /'
redis-cli --version || true

echo
echo "==> toolchain ready"
