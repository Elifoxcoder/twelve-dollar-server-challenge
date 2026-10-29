#!/usr/bin/env bash
# Run once as root on a clean Ubuntu 24.04.
set -euo pipefail
apt-get update
apt-get install -y --no-install-recommends \
  build-essential ca-certificates \
  libsqlite3-dev liburing-dev
