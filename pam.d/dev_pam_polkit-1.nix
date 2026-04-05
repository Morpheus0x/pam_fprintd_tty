# NixOS module (development): fingerprint + password auth using pam_fprintd_tty
#
# This builds the module from a local checkout of the repository.
# For production use, use sudo.nixos.example.nix (fetches from Git) instead.
#
# Usage:
#   In your configuration.nix (or wherever you manage imports):
#
#     imports = [
#       /path/to/pam-fprintd-tty/pam.d/sudo.nixos.dev.example.nix
#     ];
#
#   Then:  sudo nixos-rebuild switch

{ config, pkgs, lib, ... }:

let
  pam-fprintd-tty = pkgs.callPackage ../package.nix { };
in
{
  # ── polkit-1: same fingerprint-then-password flow ─────────────────
  security.pam.services.polkit-1 = {
    fprintAuth = false;

    rules.auth.fprintd_tty = {
      order = config.security.pam.services.polkit-1.rules.auth.unix.order - 10;
      control = "[success=done default=ignore]";
      modulePath = "${pam-fprintd-tty}/lib/security/pam_fprintd_tty.so";
      settings = {
        timeout = 10;
      };
    };
  };
}
