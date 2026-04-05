# NixOS module: fingerprint + password authentication using pam_fprintd_passwd
#
# This fetches the module source from the upstream Git repository.
# For local development, use sudo.nixos.dev.example.nix instead.
#
# Usage:
#   In your configuration.nix (or wherever you manage imports):
#
#     imports = [
#       /path/to/pam-fprintd-passwd/pam.d/sudo.nixos.example.nix
#     ];
#
#   Then:  sudo nixos-rebuild switch

{ config, pkgs, lib, ... }:

let
  pam-fprintd-passwd = pkgs.callPackage
    ((builtins.fetchGit {
      url = "https://git.example.com/myuser/pam-fprintd-passwd";
      ref = "main";
    }) + "/package.nix") { };
in
{
  # ── sudo: fingerprint first, then password ────────────────────────
  security.pam.services.sudo = {
    # Disable the stock fprintd PAM integration
    fprintAuth = false;

    # Insert our module before pam_unix with extended control:
    #   - PAM_SUCCESS          → done (fingerprint matched, skip remaining)
    #   - PAM_AUTHINFO_UNAVAIL → ignore, continue to pam_unix (password)
    #   - default              → ignore, continue to pam_unix (password)
    rules.auth.fprintd_passwd = {
      order = config.security.pam.services.sudo.rules.auth.unix.order - 10;
      control = "[success=done default=ignore]";
      modulePath = "${pam-fprintd-passwd}/lib/security/pam_fprintd_passwd.so";
      settings = {
        timeout = 10;
      };
    };
  };
}
