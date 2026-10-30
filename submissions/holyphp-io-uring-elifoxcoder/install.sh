#!/usr/bin/env bash
# Run once as root on a clean Ubuntu 24.04.
#   build-essential  — gcc, linker
#   libsqlite3-dev   — SQLite headers + library the generated server links against
set -euo pipefail
apt-get update
apt-get install -y --no-install-recommends \
  build-essential ca-certificates \
  libsqlite3-dev
