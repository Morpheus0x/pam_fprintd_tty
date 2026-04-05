# NixOS module: fingerprint + password authentication using pam_fprint_fixed
#
# This replaces the standard pam_fprintd integration with our custom module
# that provides a smoother UX: shows a prompt, accepts fingerprint OR
# Enter-key / timeout to fall back to password.
#
# Usage:
#   In your configuration.nix (or wherever you manage imports):
#
#     imports = [
#       /path/to/pam_fprint_fixed/pam.d/sudo.nixos.example.nix
#     ];
#
#   Then:  sudo nixos-rebuild switch

{ config, pkgs, lib, ... }:

let
  # Build the PAM module from the project source
  pam-fprint-fixed = pkgs.callPackage ../package.nix { };
in
{
  # ── fprintd daemon + driver ───────────────────────────────────────
  services.fprintd.enable = true;
  services.fprintd.tod.enable = true;
  services.fprintd.tod.driver = pkgs.libfprint-2-tod1-goodix;

  # ── sudo: fingerprint first, then password ────────────────────────
  security.pam.services.sudo = {
    # Disable the stock fprintd PAM integration
    fprintAuth = false;

    # Insert our module before pam_unix with "sufficient" control:
    #   - PAM_SUCCESS          → auth done (fingerprint matched)
    #   - PAM_AUTHINFO_UNAVAIL → silently continue to pam_unix (password)
    rules.auth.fprint_fixed = {
      order = config.security.pam.services.sudo.rules.auth.unix.order - 10;
      control = "sufficient";
      modulePath = "${pam-fprint-fixed}/lib/security/pam_fprint_fixed.so";
      settings = {
        timeout = 10;
      };
    };
  };

  # ── polkit-1: same fingerprint-then-password flow ─────────────────
  security.pam.services.polkit-1 = {
    fprintAuth = false;

    rules.auth.fprint_fixed = {
      order = config.security.pam.services.polkit-1.rules.auth.unix.order - 10;
      control = "sufficient";
      modulePath = "${pam-fprint-fixed}/lib/security/pam_fprint_fixed.so";
      settings = {
        timeout = 10;
      };
    };
  };
}
