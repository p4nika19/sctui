#!/usr/bin/env bash
# ==============================================================================
# SoundCloud TUI (sctui) - Complete System Uninstallation Script
# ==============================================================================
# This script cleanly and completely removes sctui, its widevine DRM satellite,
# background daemons, user configuration, track cache, and system integrations.
# ==============================================================================

set -euo pipefail

# ANSI color definitions
BOLD='\033[1m'
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
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

# Determine sudo requirement
SUDO=""
if [ "${EUID:-$(id -u)}" -ne 0 ]; then
    if command -v sudo &>/dev/null; then
        SUDO="sudo"
    elif command -v doas &>/dev/null; then
        SUDO="doas"
    fi
fi

print_banner() {
    cat << "EOF"
  ____                     _  ____ _                 _   _____ _   _ ___ 
 / ___|  ___  _   _ _ __   __| |/ ___| | ___  _   _  __| | |_   _| | | |_ _|
 \___ \ / _ \| | | | '_ \ / _` | |   | |/ _ \| | | |/ _` |   | | | | | | | | 
  ___) | (_) | |_| | | | | (_| | |___| | (_) | |_| | (_| |   | | | |_| | | | 
 |____/ \___/ \__,_|_| |_|\__,_|\____|_|\___/ \__,_|\__,_|   |_|  \___/|___|
                                                                             
                   COMPLETE SYSTEM UNINSTALLATION SCRIPT
EOF
    echo
}

show_help() {
    cat << EOF
Usage: $0 [OPTIONS]

Options:
    -y, --yes, -f, --force    Skip confirmation prompt and proceed immediately
    --keep-config             Do not delete user configuration files (~/.config/soundcloud-tui)
    --keep-cache              Do not delete cached audio tracks and metadata (~/.cache/soundcloud-tui)
    --clean-repo              Also purge build/ and node_modules/ in current repository
    -h, --help                Show this help message and exit

EOF
}

# Parse command line arguments
FORCE=false
REMOVE_CONFIG=true
REMOVE_CACHE=true
CLEAN_REPO=false

while [[ $# -gt 0 ]]; do
    case "$1" in
        -y|--yes|-f|--force)
            FORCE=true
            shift
            ;;
        --keep-config)
            REMOVE_CONFIG=false
            shift
            ;;
        --keep-cache)
            REMOVE_CACHE=false
            shift
            ;;
        --clean-repo)
            CLEAN_REPO=true
            shift
            ;;
        -h|--help)
            print_banner
            show_help
            exit 0
            ;;
        *)
            error "Unknown option: $1"
            show_help
            exit 1
            ;;
    esac
done

confirm_removal() {
    if [ "$FORCE" = true ]; then
        return 0
    fi

    echo -e "${YELLOW}${BOLD}The following components will be removed from your system:${NC}"
    echo -e "  • Installed sctui binary (/usr/local/bin/sctui or ~/.local/bin/sctui)"
    echo -e "  • Widevine DRM satellite and Electron runtime (/usr/local/share/sctui or ~/.local/share/sctui)"
    if [ "$REMOVE_CONFIG" = true ]; then
        echo -e "  • Configuration & session tokens (${XDG_CONFIG_HOME:-$HOME/.config}/soundcloud-tui)"
    else
        echo -e "  • Configuration: ${CYAN}Preserved (--keep-config)${NC}"
    fi
    if [ "$REMOVE_CACHE" = true ]; then
        echo -e "  • Cached audio tracks & metadata (${XDG_CACHE_HOME:-$HOME/.cache}/soundcloud-tui)"
    else
        echo -e "  • Track cache: ${CYAN}Preserved (--keep-cache)${NC}"
    fi
    echo -e "  • Temporary files and active daemon processes"
    echo

    read -r -p "Are you sure you want to completely remove sctui? [y/N]: " response
    case "$response" in
        [yY][eE][sS]|[yY]|д|Д|да|Да|ДА)
            return 0
            ;;
        *)
            info "Uninstallation aborted by user."
            exit 0
            ;;
    esac
}

terminate_running_processes() {
    info "Checking for running sctui instances and satellite daemons..."

    # Terminate sctui-drm-satellite or castlabs-electron
    if pgrep -f "sctui-drm-satellite" &>/dev/null; then
        info "Stopping running DRM satellite daemon..."
        pkill -f "sctui-drm-satellite" || true
        sleep 0.5
    fi

    if pgrep -f "castlabs-electron.*main.js" &>/dev/null; then
        info "Stopping Castlabs Electron process..."
        pkill -f "castlabs-electron.*main.js" || true
        sleep 0.5
    fi

    # Check for active sctui main process
    if pgrep -x "sctui" &>/dev/null; then
        warn "sctui application is currently running."
        if [ "$FORCE" = true ]; then
            pkill -x "sctui" || true
            sleep 0.5
        else
            read -r -p "Terminate active sctui process? [y/N]: " resp
            if [[ "$resp" =~ ^([yY][eE][sS]|[yY]|д|Д|да|Да|ДА)$ ]]; then
                pkill -x "sctui" || true
                sleep 0.5
            fi
        fi
    fi
    success "Process check complete."
}

remove_system_binaries() {
    info "Removing sctui binary executables..."

    local bin_paths=(
        "/usr/local/bin/sctui"
        "/usr/bin/sctui"
        "${HOME}/.local/bin/sctui"
        "${HOME}/bin/sctui"
    )

    local found_any=false
    for bin in "${bin_paths[@]}"; do
        if [ -f "$bin" ] || [ -L "$bin" ]; then
            found_any=true
            if [ -w "$(dirname "$bin")" ]; then
                rm -f "$bin"
                success "Removed binary: $bin"
            elif [ -n "$SUDO" ]; then
                $SUDO rm -f "$bin"
                success "Removed binary (elevated): $bin"
            else
                warn "Cannot remove $bin: permission denied (no sudo/doas)."
            fi
        fi
    done

    if [ "$found_any" = false ]; then
        info "No installed sctui binaries were found."
    fi
}

remove_satellite_resources() {
    info "Removing DRM satellite daemon resources..."

    local share_paths=(
        "/usr/local/share/sctui"
        "/usr/share/sctui"
        "${HOME}/.local/share/sctui"
    )

    local found_any=false
    for dir in "${share_paths[@]}"; do
        if [ -d "$dir" ]; then
            found_any=true
            if [ -w "$(dirname "$dir")" ]; then
                rm -rf "$dir"
                success "Removed satellite resources: $dir"
            elif [ -n "$SUDO" ]; then
                $SUDO rm -rf "$dir"
                success "Removed satellite resources (elevated): $dir"
            else
                warn "Cannot remove $dir: permission denied (no sudo/doas)."
            fi
        fi
    done

    if [ "$found_any" = false ]; then
        info "No shared satellite resources were found."
    fi
}

remove_user_data() {
    local config_dir="${XDG_CONFIG_HOME:-$HOME/.config}/soundcloud-tui"
    local cache_dir="${XDG_CACHE_HOME:-$HOME/.cache}/soundcloud-tui"

    if [ "$REMOVE_CONFIG" = true ]; then
        if [ -d "$config_dir" ]; then
            rm -rf "$config_dir"
            success "Removed user configuration & sessions: $config_dir"
        fi
    else
        info "Skipping user configuration (--keep-config)."
    fi

    if [ "$REMOVE_CACHE" = true ]; then
        if [ -d "$cache_dir" ]; then
            rm -rf "$cache_dir"
            success "Removed track cache and metadata: $cache_dir"
        fi
    else
        info "Skipping track cache (--keep-cache)."
    fi

    # Remove temporary runtime directories
    local tmp_dir="/tmp/soundcloud-tui"
    if [ -d "$tmp_dir" ]; then
        rm -rf "$tmp_dir"
        success "Removed temporary runtime files: $tmp_dir"
    fi

    # Clean temporary unit test artifacts if present
    rm -rf /tmp/sctui_test_* 2>/dev/null || true
}

clean_repository_artifacts() {
    if [ "$CLEAN_REPO" = true ]; then
        local script_dir
        script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
        if [ -f "${script_dir}/xmake.lua" ]; then
            info "Purging repository build artifacts and satellite node_modules..."
            rm -rf "${script_dir}/build" "${script_dir}/.xmake" "${script_dir}/satellite/node_modules"
            success "Repository cleaned successfully."
        fi
    fi
}

main() {
    print_banner
    confirm_removal
    terminate_running_processes
    remove_system_binaries
    remove_satellite_resources
    remove_user_data
    clean_repository_artifacts

    echo
    echo "============================================================================="
    success "SoundCloud TUI (sctui) has been completely removed from your system!"
    echo "============================================================================="
}

main "$@"
