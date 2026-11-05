#!/usr/bin/env bash
# Copies the shared SDK sources into the Defold extension (Defold's build server only sees the extension folder).
# CI runs this and fails if the copies were out of date.
set -euo pipefail
SDK="$(cd "$(dirname "$0")/../.." && pwd)"
EXT="$SDK/lua/defold/wavestorm"
cp "$SDK/core/include/wavestorm.h" "$EXT/include/wavestorm.h"
cp "$SDK/core/src/wavestorm.cpp" "$EXT/src/wavestorm.cpp"
cp "$SDK/lua/src/wavestorm_lua.hpp" "$EXT/src/wavestorm_lua.hpp"
