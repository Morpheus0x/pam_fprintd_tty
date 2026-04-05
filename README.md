# pam_fprintd_passwd

A custom PAM module that provides seamless fingerprint-then-password authentication for `sudo` and other PAM-aware services on Linux. It communicates with the `fprintd` daemon over D-Bus and gracefully falls back to password authentication on timeout, Ctrl+C, or fingerprint mismatch.

## Features

- **Fingerprint first, password fallback** -- prompts for fingerprint, falls through to the standard password prompt on failure
- **Ctrl+C support** -- press Ctrl+C during the fingerprint prompt to immediately switch to password (works even though `sudo` blocks SIGINT during PAM authentication)
- **Multiple attempts** -- silently retries fingerprint verification up to 3 times with a grace period between attempts to avoid rapid-fire failures
- **Configurable timeout** -- per-attempt timeout (default 10 seconds) resets after each retry
- **Graceful degradation** -- if `fprintd` is not running or no fingerprints are enrolled, silently falls through to password without errors
- **Terminal safety** -- terminal settings are always restored, even on signals (SIGTERM, SIGHUP)
- **D-Bus native** -- talks directly to `fprintd` via D-Bus, no subprocess spawning

## Dependencies

- `libpam` development headers
- `libdbus-1` development headers
- `pkg-config`
- `fprintd` (runtime)

## Building

### NixOS (recommended)

```bash
nix-build
```

The resulting `.so` is at `result/lib/security/pam_fprintd_passwd.so`.

For development, enter a shell with all dependencies:

```bash
nix-shell
make
```

### Other distributions

```bash
make
sudo make install
```

The Makefile auto-detects the correct PAM module directory for your distribution (Debian/Ubuntu, Arch, Fedora, etc.). Override manually if needed:

```bash
sudo make install PAMDIR=/usr/lib/security
```

## Configuration

### NixOS

Import the provided NixOS module in your `configuration.nix`:

```nix
imports = [
  /path/to/pam-fprintd-passwd/pam.d/pam_sudo.nix
];
```

For local development, use the dev variant instead:

```nix
imports = [
  /path/to/pam-fprintd-passwd/pam.d/dev_pam_sudo.nix
];
```

Then rebuild:

```bash
sudo nixos-rebuild switch
```

### Other distributions

After `make install`, edit `/etc/pam.d/sudo` (see `pam.d/sudo.example` for a complete example):

```
auth  [success=2 default=ignore]  pam_fprintd_passwd.so  timeout=10
auth  [success=1 default=ignore]  pam_unix.so            nullok try_first_pass
auth  requisite                    pam_deny.so
auth  required                     pam_permit.so

account  include  system-auth
session  include  system-auth
```

### Module arguments

| Argument    | Description                                      | Default |
|-------------|--------------------------------------------------|---------|
| `timeout=N` | Seconds to wait for fingerprint (per attempt)    | `10`    |
| `debug`     | Enable detailed diagnostics in syslog            | off     |

Debug logs can be viewed with:

```bash
journalctl --since "5 min ago" --grep pam_fprintd_passwd
```

## Project structure

```
src/
  pam_fprintd_passwd.c   Main PAM module (poll loop, terminal handling, retry logic)
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
