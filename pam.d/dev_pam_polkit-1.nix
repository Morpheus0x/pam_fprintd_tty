# NixOS module (development): fingerprint + password auth using pam_fprintd_passwd
#
# This builds the module from a local checkout of the repository.
# For production use, use sudo.nixos.example.nix (fetches from Git) instead.
#
# Usage:
#   In your configuration.nix (or wherever you manage imports):
#
#     imports = [
#       /path/to/pam-fprintd-passwd/pam.d/sudo.nixos.dev.example.nix
#     ];
#
#   Then:  sudo nixos-rebuild switch

{ config, pkgs, lib, ... }:

let
  pam-fprintd-passwd = pkgs.callPackage ../package.nix { };
in
{
  # ── polkit-1: same fingerprint-then-password flow ─────────────────
  security.pam.services.polkit-1 = {
    fprintAuth = false;

    rules.auth.fprintd_passwd = {
      order = config.security.pam.services.polkit-1.rules.auth.unix.order - 10;
      control = "[success=done default=ignore]";
      modulePath = "${pam-fprintd-passwd}/lib/security/pam_fprintd_passwd.so";
      settings = {
        timeout = 10;
      };
    };
  };
}
