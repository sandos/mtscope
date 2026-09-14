#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

printf '%s\n' 'Running ASan/UBSan verification'
cmake --preset asan
cmake --build --preset asan --target asan

printf '%s\n' 'Running Valgrind verification'
cmake --preset valgrind
cmake --build --preset valgrind --target valgrind

printf '%s\n' 'ASan/UBSan and Valgrind verification passed'
