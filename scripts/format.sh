#!/usr/bin/env bash
# Format every C++ source + header in-place with clang-format.

set -eu
cd "$(dirname "$0")/.."

if ! command -v clang-format >/dev/null 2>&1; then
  echo "clang-format not found" >&2
  exit 1
fi

find include src adapters binaries tests \
  -type f \( -name '*.h' -o -name '*.cc' \) \
  -not -path 'build/*' \
  -not -path 'build-debug/*' \
  -not -path 'build-aarch64/*' \
  -exec clang-format -i {} +
