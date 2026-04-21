#!/usr/bin/env bash
# Run cpplint across the public headers + sources.
# Exit non-zero on any lint error.

set -eu
cd "$(dirname "$0")/.."

if ! command -v cpplint >/dev/null 2>&1; then
  echo "cpplint not found; install it with 'pip install cpplint'" >&2
  exit 1
fi

mapfile -t files < <(
  find include src adapters binaries tests \
    -type f \( -name '*.h' -o -name '*.cc' \) \
    -not -path 'build/*' \
    -not -path 'build-debug/*' \
    -not -path 'build-aarch64/*'
)

if [[ ${#files[@]} -eq 0 ]]; then
  echo "no sources found" >&2
  exit 0
fi

cpplint --quiet "${files[@]}"
