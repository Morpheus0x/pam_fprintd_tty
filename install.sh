#!/usr/bin/env bash
# install.sh – install pam_fprint_fixed.so and optionally configure PAM
#
# Usage:
#   sudo ./install.sh              install .so only
#   sudo ./install.sh --with-pam   also install /etc/pam.d/sudo config
#   sudo ./install.sh --uninstall  remove .so and restore backed-up config
#
# On NixOS the PAM config is managed declaratively; use the --with-pam
# flag only on traditional distros (Arch, Fedora, Debian …).

set -euo pipefail

# ── Configuration ─────────────────────────────────────────────────────

MODULE_NAME="pam_fprint_fixed.so"
BUILD_SO="./${MODULE_NAME}"
PAM_EXAMPLE="./pam.d/sudo.example"

# Detect PAM module directory.
# Order: explicit env var → multilib path → /usr/lib/security (Arch/Fedora)
if [[ -n "${PAM_MODULE_DIR:-}" ]]; then
    INSTALL_DIR="$PAM_MODULE_DIR"
elif [[ -d "/usr/lib/x86_64-linux-gnu/security" ]]; then
    INSTALL_DIR="/usr/lib/x86_64-linux-gnu/security"     # Debian / Ubuntu
elif [[ -d "/usr/lib64/security" ]]; then
    INSTALL_DIR="/usr/lib64/security"                     # Fedora 64-bit
elif [[ -d "/usr/lib/security" ]]; then
    INSTALL_DIR="/usr/lib/security"                       # Arch / generic
else
    INSTALL_DIR="/usr/lib/security"                       # fallback
fi

PAM_CONF="/etc/pam.d/sudo"
BACKUP_SUFFIX=".bak.pam_fprint_fixed"

# ── Colours ───────────────────────────────────────────────────────────

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'

info()  { echo -e "${GREEN}[+]${NC} $*"; }
warn()  { echo -e "${YELLOW}[!]${NC} $*"; }
err()   { echo -e "${RED}[-]${NC} $*" >&2; }

# ── Helpers ───────────────────────────────────────────────────────────

check_root() {
    if [[ $EUID -ne 0 ]]; then
        err "This script must be run as root (sudo ./install.sh)"
        exit 1
    fi
}

check_built() {
    if [[ ! -f "$BUILD_SO" ]]; then
        err "${MODULE_NAME} not found. Run 'make' first."
        exit 1
    fi
}

detect_nixos() {
    [[ -f /etc/NIXOS ]] || [[ -d /run/current-system/sw ]]
}

# ── Install ───────────────────────────────────────────────────────────

do_install_so() {
    check_root
    check_built

    info "Installing ${MODULE_NAME} → ${INSTALL_DIR}/"
    mkdir -p "$INSTALL_DIR"
    install -m 0755 "$BUILD_SO" "${INSTALL_DIR}/${MODULE_NAME}"
    info "Installed successfully."

    if detect_nixos; then
        warn "NixOS detected.  PAM configuration is managed declaratively."
        warn "Add the following to your NixOS configuration.nix:"
        echo ""
        echo "  security.pam.services.sudo = {"
        echo "    text = ''"
        echo "      auth  [success=2 default=ignore]  ${INSTALL_DIR}/${MODULE_NAME}  timeout=10"
        echo "      auth  [success=1 default=ignore]  pam_unix.so  nullok try_first_pass"
        echo "      auth  requisite                    pam_deny.so"
        echo "      auth  required                     pam_permit.so"
        echo "      account required pam_unix.so"
        echo "      session required pam_limits.so"
        echo "      session required pam_unix.so"
        echo "    '';"
        echo "  };"
        echo ""
        warn "Then run: sudo nixos-rebuild switch"
    fi
}

do_install_pam() {
    if detect_nixos; then
        err "Cannot install PAM config on NixOS (it's declaratively managed)."
        err "See the NixOS snippet printed by './install.sh' (without --with-pam)."
        exit 1
    fi

    if [[ ! -f "$PAM_EXAMPLE" ]]; then
        err "Example PAM config not found at ${PAM_EXAMPLE}"
        exit 1
    fi

    if [[ -f "$PAM_CONF" ]]; then
        info "Backing up existing ${PAM_CONF} → ${PAM_CONF}${BACKUP_SUFFIX}"
        cp -a "$PAM_CONF" "${PAM_CONF}${BACKUP_SUFFIX}"
    fi

    info "Installing PAM config → ${PAM_CONF}"
    install -m 0644 "$PAM_EXAMPLE" "$PAM_CONF"
    info "PAM configuration installed."
    warn "Test with: sudo -k && sudo echo ok"
    warn "If locked out, use the backup: cp ${PAM_CONF}${BACKUP_SUFFIX} ${PAM_CONF}"
}

# ── Uninstall ─────────────────────────────────────────────────────────

do_uninstall() {
    check_root

    if [[ -f "${INSTALL_DIR}/${MODULE_NAME}" ]]; then
        info "Removing ${INSTALL_DIR}/${MODULE_NAME}"
        rm -f "${INSTALL_DIR}/${MODULE_NAME}"
    else
        warn "${MODULE_NAME} not found in ${INSTALL_DIR}/, nothing to remove."
    fi

    if [[ -f "${PAM_CONF}${BACKUP_SUFFIX}" ]]; then
        info "Restoring ${PAM_CONF} from backup"
        cp -a "${PAM_CONF}${BACKUP_SUFFIX}" "$PAM_CONF"
        rm -f "${PAM_CONF}${BACKUP_SUFFIX}"
    fi

    info "Uninstall complete."
}

# ── Main ──────────────────────────────────────────────────────────────

case "${1:-}" in
    --with-pam)
        do_install_so
        do_install_pam
        ;;
    --uninstall)
        do_uninstall
        ;;
    *)
        do_install_so
        ;;
esac
