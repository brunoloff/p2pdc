#!/usr/bin/env bash
# Installs the Emscripten version the browser builds use into FOLDER (default: .emsdk).
# Afterwards, run: source FOLDER/emsdk_env.sh
set -euo pipefail

readonly version=4.0.7
folder=${1:-.emsdk}

if [ ! -d "$folder/.git" ]; then
    git -c advice.detachedHead=false clone --depth 1 --branch "$version" \
        https://github.com/emscripten-core/emsdk.git "$folder"
fi
"$folder/emsdk" install "$version"
"$folder/emsdk" activate "$version"
