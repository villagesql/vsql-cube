#!/usr/bin/env bash
# Copyright (c) VillageSQL Contributors
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 2.0,
# as published by the Free Software Foundation.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [[ -z "${VillageSQL_BUILD_DIR:-}" ]]; then
  echo "ERROR: VillageSQL_BUILD_DIR is not set." >&2
  echo "  export VillageSQL_BUILD_DIR=/path/to/villagesql/build" >&2
  exit 1
fi

BUILD_DIR="${SCRIPT_DIR}/build"
mkdir -p "${BUILD_DIR}"

cmake -S "${SCRIPT_DIR}" -B "${BUILD_DIR}" \
  -DVillageSQL_BUILD_DIR="${VillageSQL_BUILD_DIR}"

cmake --build "${BUILD_DIR}" -- -j"$(sysctl -n hw.logicalcpu 2>/dev/null || nproc)"

echo ""
echo "Build complete. VEB file:"
ls -lh "${BUILD_DIR}/"*.veb 2>/dev/null || \
  find "${BUILD_DIR}" -name "*.veb" -print
