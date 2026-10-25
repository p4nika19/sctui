#!/usr/bin/env bash
# ==============================================================================
# One-line universal installer for SoundCloud TUI (sctui):
#   curl -fsSL https://raw.githubusercontent.com/p4nika19/sctui/main/get.sh | bash
#
# Downloads or clones the sctui repository, installs dependencies, builds the
# release binary with xmake, configures the Widevine DRM satellite, and installs
# sctui to your system.
#
# Pass options after "bash -s --", e.g.:
#   curl -fsSL https://raw.githubusercontent.com/p4nika19/sctui/main/get.sh | bash -s -- --user
# ==============================================================================
set -euo pipefail

REPO="${SCTUI_REPO:-p4nika19/sctui}"
BRANCH="${SCTUI_BRANCH:-main}"

ARCH=$(uname -m)
if [ "$ARCH" != "x86_64" ]; then
    echo "[!] Warning: Native optimized SIMD builds are targeted for x86_64 Linux (detected: $ARCH)." >&2
fi

for tool in curl tar; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "[ERROR] Required tool '$tool' is not installed. Please install it and try again." >&2
        exit 1
    fi
done

TMP_DIR=$(mktemp -d "${TMPDIR:-/tmp}/sctui-get-XXXXXX")
trap 'rm -rf "$TMP_DIR"' EXIT

if command -v git >/dev/null 2>&1; then
    echo "==> Cloning sctui from https://github.com/$REPO.git (branch: $BRANCH)..."
    git clone --depth 1 --branch "$BRANCH" "https://github.com/$REPO.git" "$TMP_DIR"
else
    echo "==> Fetching sctui source archive from GitHub..."
    curl -fsSL "https://github.com/$REPO/archive/refs/heads/$BRANCH.tar.gz" | tar -xz -C "$TMP_DIR" --strip-components=1
fi

if [ -e /dev/tty ]; then
    exec bash "$TMP_DIR/install.sh" "$@" </dev/tty
else
    exec bash "$TMP_DIR/install.sh" "$@"
fi
