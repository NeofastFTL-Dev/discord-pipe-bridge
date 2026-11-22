#!/usr/bin/env bash
# Build the Wine <-> Linux Discord IPC bridge.
#
# Needs winegcc (package: wine-devel / wine-staging-tools, ships with wine) and
# gcc. The build produces two files that must stay together: the PE side
# (dist/discord-pipe-bridge.exe) and its unix side (.exe.so) that performs the
# real AF_UNIX socket connection.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if ! command -v winegcc >/dev/null 2>&1; then
  echo "winegcc not found - install the Wine development tools (wine-devel or wine-staging) first" >&2
  exit 1
fi

mkdir -p "$HERE/dist"
# shellcheck disable=SC2086
winegcc -O2 -Wall -Wextra -o "$HERE/dist/discord-pipe-bridge.exe" "$HERE/main.c"

ls -l "$HERE/dist/discord-pipe-bridge.exe" "$HERE/dist/discord-pipe-bridge.exe.so"
