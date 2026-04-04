# NixOS configuration snippet for pam_fprint_fixed
#
# Add this to your /etc/nixos/configuration.nix (or a separate module)
# and run: sudo nixos-rebuild switch
#
# Prerequisites:
#   - fprintd must be enabled:  services.fprintd.enable = true;
#   - The .so must be built and installed to the path below.
#     Adjust the path if you installed it elsewhere.

{ config, lib, pkgs, ... }:

let
  # Path where install.sh placed the module (default for Arch-style)
  pamModulePath = "/usr/lib/security/pam_fprint_fixed.so";
in
{
  # Enable fprintd
  services.fprintd.enable = true;

  # Override the sudo PAM service
  security.pam.services.sudo = {
    text = ''
      # pam_fprint_fixed: fingerprint first, then password fallback
      auth  [success=2 default=ignore]  ${pamModulePath}  timeout=10
      auth  [success=1 default=ignore]  pam_unix.so  nullok try_first_pass
      auth  requisite                    pam_deny.so
      auth  required                     pam_permit.so

      account required pam_unix.so
      session required pam_limits.so
      session required pam_unix.so
    '';
  };
}
