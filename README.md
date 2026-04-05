# pam_fprintd_tty

A custom PAM module for fingerprint authentication via `fprintd` that lets you press Ctrl+C to abort and fall back to password. Designed for `sudo` and other terminal-based PAM services where `/dev/tty` is available. The standard `pam_fprintd` module blocks until timeout with no way to cancel mid-scan — this module fixes that by opening the TTY in raw mode and using `poll()` to detect keypresses during the fingerprint scan.

## Features

- **Ctrl+C to abort** -- press Ctrl+C during the fingerprint scan to immediately skip to password authentication (works even though `sudo` blocks SIGINT during PAM authentication)
- **Multiple attempts** -- retries fingerprint verification up to 3 times with a grace period between attempts to avoid rapid-fire failures
- **Configurable timeout** -- per-attempt timeout (default 10 seconds) resets after each retry
- **Graceful degradation** -- if `fprintd` is not running or no fingerprints are enrolled, silently falls through to password without errors
- **Terminal safety** -- terminal settings are always restored, even on signals (SIGTERM, SIGHUP)
- **D-Bus native** -- talks directly to `fprintd` via D-Bus, no subprocess spawning

## Installation

### NixOS

Download [`pam.d/pam_sudo.nix`](pam.d/pam_sudo.nix) and import it in your `configuration.nix`. That's it — Nix will fetch the source from Git, build the module, and configure the PAM stack automatically:

```nix
imports = [
  /path/to/pam_sudo.nix
];
```

Then rebuild:

```bash
sudo nixos-rebuild switch
```

The module disables the stock `pam_fprintd` integration and inserts `pam_fprintd_tty` before `pam_unix` in the `sudo` PAM service. You can adjust the timeout and enable debug logging by editing the `settings` block inside the file.

#### Local development (NixOS)

For iterating on the module source locally, use [`pam.d/dev_pam_sudo.nix`](pam.d/dev_pam_sudo.nix) instead. It builds from a local checkout rather than fetching from Git, so changes take effect on the next `nixos-rebuild switch`:

```nix
imports = [
  /path/to/pam-fprintd-tty/pam.d/dev_pam_sudo.nix
];
```

You can also build the `.so` directly or enter a development shell:

```bash
nix-build                # → result/lib/security/pam_fprintd_tty.so
nix-shell                # drops you into a shell with all deps
make                     # build inside nix-shell
```

### Other distributions

#### Dependencies

- `libpam` development headers
- `libdbus-1` development headers
- `pkg-config`
- `fprintd` (runtime)

#### Build and install

```bash
make
sudo make install
```

The Makefile auto-detects the correct PAM module directory for your distribution (Debian/Ubuntu, Arch, Fedora, etc.). Override manually if needed:

```bash
sudo make install PAMDIR=/usr/lib/security
```

#### PAM configuration

After installing, edit `/etc/pam.d/sudo` (see [`pam.d/sudo.example`](pam.d/sudo.example) for a complete example):

```
auth  [success=2 default=ignore]  pam_fprintd_tty.so  timeout=10
auth  [success=1 default=ignore]  pam_unix.so            nullok try_first_pass
auth  requisite                    pam_deny.so
auth  required                     pam_permit.so

account  include  system-auth
session  include  system-auth
```

## Module arguments

| Argument    | Description                                      | Default |
|-------------|--------------------------------------------------|---------|
| `timeout=N` | Seconds to wait for fingerprint (per attempt)    | `10`    |
| `debug`     | Enable detailed diagnostics in syslog            | off     |

Debug logs can be viewed with:

```bash
journalctl --since "5 min ago" --grep pam_fprintd_tty
```

## Project structure

```
src/
  pam_fprintd_tty.c      Main PAM module (poll loop, terminal handling, retry logic)
  fprintd_dbus.c         D-Bus helpers for fprintd communication
  fprintd_dbus.h         Header for D-Bus helpers
pam.d/
  sudo.example           Example /etc/pam.d/sudo for non-NixOS
  pam_sudo.nix           NixOS module (fetches from Git)
  dev_pam_sudo.nix       NixOS module (local source, for development)
Makefile                 Build system with auto-detected install paths
package.nix              Nix package expression
default.nix              Wrapper for nix-build
shell.nix                Nix development shell
```

## Contributing

Contributions are welcome. Please open an issue or pull request on the project repository.

When modifying the C source, ensure the code compiles cleanly with:

```bash
make clean && make
```

The project uses `-Wall -Wextra -Wpedantic -Werror`, so all warnings are treated as errors.

## Disclaimer

This project was created with the assistance of AI (Claude Opus 4.6 Thinking). While the code has been reviewed and tested, please audit it carefully before deploying in security-sensitive environments. PAM modules run with elevated privileges and directly affect system authentication.
