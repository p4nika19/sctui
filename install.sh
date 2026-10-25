#!/usr/bin/env bash
# ==============================================================================
# SoundCloud TUI (sctui) - Universal Automated Installation Script
# ==============================================================================
# This script automatically detects your distribution's package manager,
# installs all required build tools and libraries, bootstraps xmake, builds
# the optimized release binary, sets up the Castlabs DRM engine, and integrates
# the program into your system so it can be launched with a single `sctui` command.
# ==============================================================================

set -euo pipefail

# ANSI color definitions
BOLD='\033[1m'
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

info() {
    echo -e "${CYAN}${BOLD}[INFO]${NC} $*"
}

success() {
    echo -e "${GREEN}${BOLD}[SUCCESS]${NC} $*"
}

warn() {
    echo -e "${YELLOW}${BOLD}[WARNING]${NC} $*"
}

error() {
    echo -e "${RED}${BOLD}[ERROR]${NC} $*" >&2
}

fatal() {
    error "$*"
    exit 1
}

# Determine sudo requirement
SUDO=""
if [ "${EUID:-$(id -u)}" -ne 0 ]; then
    if command -v sudo &>/dev/null; then
        SUDO="sudo"
    elif command -v doas &>/dev/null; then
        SUDO="doas"
    fi
fi

# Detect Package Manager
detect_pm() {
    if command -v pacman &>/dev/null; then
        echo "pacman"
    elif command -v apt-get &>/dev/null; then
        echo "apt"
    elif command -v dnf &>/dev/null; then
        echo "dnf"
    elif command -v zypper &>/dev/null; then
        echo "zypper"
    elif command -v apk &>/dev/null; then
        echo "apk"
    elif command -v xbps-install &>/dev/null; then
        echo "xbps"
    elif command -v emerge &>/dev/null; then
        echo "emerge"
    elif command -v brew &>/dev/null; then
        echo "brew"
    else
        echo "unknown"
    fi
}

print_banner() {
    cat << "EOF"
  ____                     _  ____ _                 _   _____ _   _ ___ 
 / ___|  ___  _   _ _ __   __| |/ ___| | ___  _   _  __| | |_   _| | | |_ _|
 \___ \ / _ \| | | | '_ \ / _` | |   | |/ _ \| | | |/ _` |   | | | | | | | | 
  ___) | (_) | |_| | | | | (_| | |___| | (_) | |_| | (_| |   | | | |_| | | | 
 |____/ \___/ \__,_|_| |_|\__,_|\____|_|\___/ \__,_|\__,_|   |_|  \___/ |___|
                                                                             
           High-Performance Native C++23 SoundCloud TUI Client
=============================================================================
EOF
}

print_manual_fallback() {
    cat << EOF
${YELLOW}${BOLD}No supported package manager automatically detected.${NC}

Please manually install the following development packages using your package manager:
  ${BOLD}1. C++23 Compiler & Linker:${NC} Clang (LLVM 18+ recommended) and mold linker
  ${BOLD}2. Build Tools:${NC}           git, curl, make
  ${BOLD}3. Node.js & npm:${NC}         Node.js 18+ and npm (for Widevine DRM engine)
  ${BOLD}4. Network & Crypto:${NC}      libcurl (dev headers), OpenSSL (dev headers)
  ${BOLD}5. Audio Libraries:${NC}       ALSA (libasound dev headers), PulseAudio (libpulse dev headers)
  ${BOLD}6. Databases & Init:${NC}      SQLite3 (dev headers), systemd / elogind (dev headers)

EOF
}

install_dependencies() {
    local pm="$1"
    info "Installing system dependencies via ${BOLD}${pm}${NC}..."

    case "$pm" in
        pacman)
            $SUDO pacman -Sy --needed --noconfirm clang mold git curl nodejs npm libpulse alsa-lib sqlite openssl base-devel || fatal "Failed to install dependencies with pacman."
            ;;
        apt)
            $SUDO apt-get update
            $SUDO apt-get install -y clang mold git curl nodejs npm libcurl4-openssl-dev libasound2-dev libpulse-dev libsystemd-dev libsqlite3-dev libssl-dev build-essential || fatal "Failed to install dependencies with apt-get."
            ;;
        dnf)
            $SUDO dnf install -y clang mold git curl nodejs npm libcurl-devel alsa-lib-devel pulseaudio-libs-devel systemd-devel sqlite-devel openssl-devel gcc-c++ make || fatal "Failed to install dependencies with dnf."
            ;;
        zypper)
            $SUDO zypper install -y -t package clang mold git curl nodejs npm libcurl-devel alsa-devel libpulse-devel systemd-devel sqlite3-devel libopenssl-devel gcc-c++ make || fatal "Failed to install dependencies with zypper."
            ;;
        apk)
            $SUDO apk add clang mold git curl nodejs npm curl-dev alsa-lib-dev pulseaudio-dev sqlite-dev openssl-dev build-base || fatal "Failed to install dependencies with apk."
            ;;
        xbps)
            $SUDO xbps-install -Sy clang mold git curl nodejs sqlite-devel libcurl-devel alsa-lib-devel pulseaudio-devel openssl-devel base-devel || fatal "Failed to install dependencies with xbps-install."
            ;;
        emerge)
            $SUDO emerge -v sys-devel/clang sys-devel/mold dev-vcs/git net-misc/curl net-libs/nodejs media-libs/alsa-lib media-libs/libpulse dev-db/sqlite dev-libs/openssl || fatal "Failed to install dependencies with emerge."
            ;;
        brew)
            brew install llvm mold git curl node sqlite openssl || fatal "Failed to install dependencies with brew."
            ;;
        unknown)
            print_manual_fallback
            read -r -p "Have you installed the required dependencies and wish to continue? [y/N] " confirm
            if [[ ! "$confirm" =~ ^[Yy]$ ]]; then
                fatal "Installation aborted. Please install required dependencies and run this script again."
            fi
            ;;
    esac
    success "System dependencies successfully installed!"
}

ensure_xmake() {
    export XMAKE_ROOT=y
    if ! command -v xmake &>/dev/null; then
        info "xmake build system was not found. Installing xmake..."
        curl -fsSL https://xmake.io/shget.text | bash
        export PATH="$HOME/.local/bin:$PATH"
        if [ -f "$HOME/.xmake/profile" ]; then
            # shellcheck source=/dev/null
            source "$HOME/.xmake/profile" 2>/dev/null || true
        fi
        if ! command -v xmake &>/dev/null; then
            fatal "xmake installation finished but xmake binary was not found in PATH ($PATH)."
        fi
    fi
    info "Found xmake version: $(xmake --version | head -n 1)"
}

build_sctui() {
    info "Building sctui in optimized Release mode..."
    # xmake.lua automatically configures clang toolchain, mold linker, and aggressive SIMD/LTO flags
    xmake config -m release -y
    xmake build -y sctui
    success "sctui binary assembled successfully!"
}

setup_satellite() {
    local script_dir
    script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    local sat_dir="${script_dir}/satellite"

    if [ -d "$sat_dir" ]; then
        info "Setting up Widevine DRM Castlabs Electron satellite in ${sat_dir}..."
        (
            cd "$sat_dir"
            if [ ! -d "node_modules" ] || ! npm list electron &>/dev/null; then
                info "Downloading required Castlabs Electron release (v44.9.3+wvcus)..."
                npm install --allow-git=all "https://github.com/castlabs/electron-releases#v44.9.3+wvcus" --save || {
                    warn "Automatic Castlabs Electron pre-download encountered an issue. sctui will attempt auto-download on first DRM launch."
                }
            else
                success "Castlabs Electron is already configured in satellite."
            fi
        )
    fi
}

install_system() {
    local script_dir
    script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    local bin_source="${script_dir}/build/linux/x86_64/release/sctui"

    if [ ! -f "$bin_source" ]; then
        # Try finding anywhere under build/
        bin_source="$(find "${script_dir}/build" -type f -name sctui -perm -111 2>/dev/null | head -n 1 || true)"
    fi

    if [ ! -f "$bin_source" ]; then
        fatal "Built sctui binary not found in build directory. Build may have failed."
    fi

    info "Installing sctui binary and satellite resources to system..."

    local bin_dest="/usr/local/bin/sctui"
    local share_dest="/usr/local/share/sctui/satellite"
    local use_sudo=false

    if [ -w "/usr/local/bin" ]; then
        use_sudo=false
    elif [ -n "$SUDO" ]; then
        use_sudo=true
    else
        # Fall back to user-local directories if no root privileges
        bin_dest="$HOME/.local/bin/sctui"
        share_dest="$HOME/.local/share/sctui/satellite"
        use_sudo=false
        mkdir -p "$HOME/.local/bin" "$HOME/.local/share/sctui"
    fi

    if [ "$use_sudo" = true ]; then
        $SUDO mkdir -p "/usr/local/bin" "/usr/local/share/sctui"
        $SUDO install -m 755 "$bin_source" "$bin_dest"
        if [ -d "${script_dir}/satellite" ]; then
            $SUDO rm -rf "$share_dest"
            $SUDO cp -r "${script_dir}/satellite" "$share_dest"
        fi
    else
        mkdir -p "$(dirname "$bin_dest")" "$(dirname "$share_dest")"
        install -m 755 "$bin_source" "$bin_dest"
        if [ -d "${script_dir}/satellite" ]; then
            rm -rf "$share_dest"
            cp -r "${script_dir}/satellite" "$share_dest"
        fi
    fi

    success "Installed binary: ${BOLD}${bin_dest}${NC}"
    if [ -d "$share_dest" ]; then
        success "Installed satellite: ${BOLD}${share_dest}${NC}"
    fi

    # Check if bin_dest directory is in PATH
    local bin_dir
    bin_dir="$(dirname "$bin_dest")"
    if [[ ":$PATH:" != *":$bin_dir:"* ]]; then
        warn "'$bin_dir' is not currently in your PATH environment variable."
        warn "You can add it by appending: export PATH=\"$bin_dir:\$PATH\" to your ~/.bashrc or ~/.zshrc."
    fi
}

main() {
    print_banner
    local pm
    pm="$(detect_pm)"
    info "Detected package manager: ${BOLD}${pm}${NC}"

    install_dependencies "$pm"
    ensure_xmake
    build_sctui
    setup_satellite
    install_system

    echo
    echo "============================================================================="
    success "SoundCloud TUI (sctui) has been successfully assembled and installed!"
    echo -e "You can now launch the application from anywhere by simply running: ${BOLD}${GREEN}sctui${NC}"
    echo "============================================================================="
}

main "$@"
