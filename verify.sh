#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

printf '%s\n' 'Running ASan/UBSan verification'
cmake --preset asan
cmake --build --preset asan --target asan meshat-monitor
MTSCOPE_E2E_BINARY=./build/asan/meshat-monitor npm run test:e2e

printf '%s\n' 'Running Valgrind verification'
cmake --preset valgrind
cmake --build --preset valgrind --target valgrind meshat-monitor
MTSCOPE_E2E_RUNNER='valgrind --tool=memcheck --leak-check=full --show-leak-kinds=definite,indirect --errors-for-leak-kinds=definite,indirect --error-exitcode=99' \
MTSCOPE_E2E_BINARY=./build/valgrind/meshat-monitor npm run test:e2e

printf '%s\n' 'ASan/UBSan and Valgrind verification passed'
