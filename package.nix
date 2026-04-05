{
  stdenv,
  lib,
  pam,
  dbus,
  pkg-config,
}:

stdenv.mkDerivation {
  pname = "pam-fprintd-tty";
  version = "0.1.0";

  src = lib.cleanSource ./.;

  nativeBuildInputs = [ pkg-config ];
  buildInputs = [ pam dbus ];

  # The Makefile already uses pkg-config for dbus-1 and links -lpam
  makeFlags = [ "CC=${stdenv.cc.targetPrefix}cc" ];

  installPhase = ''
    runHook preInstall
    mkdir -p $out/lib/security
    install -m 0755 build/pam_fprintd_tty.so $out/lib/security/
    runHook postInstall
  '';

  meta = with lib; {
    description = "PAM module for seamless fingerprint-then-password authentication via fprintd";
    homepage = "https://git.example.com/myuser/pam-fprintd-tty";
    license = licenses.mit;
    platforms = platforms.linux;
  };
}
