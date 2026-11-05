#!/usr/bin/env bash
# Wrapper so the rea CLI runs with the toolchain this project was set up with.
export PATH="$HOME/.local/opt/node-v26.13.0-linux-x64/bin:$PATH"
export GHIDRA_INSTALL_DIR="${GHIDRA_INSTALL_DIR:-$HOME/.local/opt/ghidra_12.1.4_PUBLIC}"
export JAVA_HOME="${JAVA_HOME:-/usr/lib/jvm/java-21-openjdk-amd64}"
export REA_GHIDRA_STARTUP_TIMEOUT_MS="${REA_GHIDRA_STARTUP_TIMEOUT_MS:-1800000}"
exec rea "$@"
