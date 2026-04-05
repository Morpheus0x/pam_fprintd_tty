# pam_fprintd_tty – Build a shared PAM module (.so)
#
# Dependencies:
#   - libpam development headers  (pam package)
#   - libdbus-1 development headers (dbus package)
#   - pkg-config
#
# Usage:
#   make            – build pam_fprintd_tty.so
#   make clean      – remove build artefacts
#   make debug      – build with debug symbols, no optimisation
#   make install    – install .so into DESTDIR/PAMDIR (needs root)
#   make uninstall  – remove installed .so
#
# Install variables (override as needed):
#   DESTDIR         – staging prefix for packaging (default: empty)
#   PAMDIR          – PAM module directory (auto-detected, or override)
#
# On NixOS, use `nix-build` instead of `make install`.

CC       ?= gcc

# dbus-1 always has a .pc file; pam usually does not
DBUS_CFLAGS := $(shell pkg-config --cflags dbus-1 2>/dev/null)
DBUS_LIBS   := $(shell pkg-config --libs   dbus-1 2>/dev/null)

CFLAGS   := -std=c11 -Wall -Wextra -Wpedantic -Werror \
            -fPIC -D_GNU_SOURCE \
            $(DBUS_CFLAGS)

# Release flags (overridden by `make debug`)
OPT      ?= -O2

LDFLAGS  := -shared -Wl,--no-undefined
LIBS     := $(DBUS_LIBS) -lpam

SRCDIR   := src
BUILDDIR := build
SRC      := $(SRCDIR)/pam_fprintd_tty.c $(SRCDIR)/fprintd_dbus.c
OBJ      := $(patsubst $(SRCDIR)/%.c,$(BUILDDIR)/%.o,$(SRC))
DEP      := $(OBJ:.o=.d)
TARGET   := $(BUILDDIR)/pam_fprintd_tty.so
MODULE   := pam_fprintd_tty.so

# Install paths
DESTDIR  ?=

# Auto-detect PAM module directory unless overridden.
# Distro-specific paths (checked in order):
#   Debian/Ubuntu  /lib/x86_64-linux-gnu/security
#   Arch/Fedora    /usr/lib/security
#   Fedora 64-bit  /usr/lib64/security
#   Generic        /lib/security
PAMDIR   ?= $(shell \
  if   [ -d /lib/x86_64-linux-gnu/security ]; then echo /lib/x86_64-linux-gnu/security; \
  elif [ -d /usr/lib64/security ];            then echo /usr/lib64/security; \
  elif [ -d /usr/lib/security ];              then echo /usr/lib/security; \
  else                                             echo /lib/security; \
  fi)

# ── Targets ───────────────────────────────────────────────────────────

.PHONY: all clean debug install uninstall

all: $(TARGET)

debug: OPT := -O0 -g3 -DDEBUG
debug: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

$(BUILDDIR)/%.o: $(SRCDIR)/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(OPT) -MMD -MP -c -o $@ $<

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

clean:
	rm -rf $(BUILDDIR)

install: $(TARGET)
	@if [ -f /etc/NIXOS ] || [ -d /run/current-system/sw ]; then \
		echo ""; \
		echo "ERROR: NixOS detected – 'make install' is not supported on NixOS."; \
		echo ""; \
		echo "Instead, use the NixOS module:"; \
		echo "  1. Add pam.d/sudo.nixos.example.nix to your configuration imports"; \
		echo "  2. Run: sudo nixos-rebuild switch"; \
		echo ""; \
		echo "Or build with Nix directly:"; \
		echo "  nix-build"; \
		echo ""; \
		exit 1; \
	fi
	install -d $(DESTDIR)$(PAMDIR)
	install -m 0755 $(TARGET) $(DESTDIR)$(PAMDIR)/$(MODULE)
	@echo ""
	@echo "Installed $(MODULE) to $(DESTDIR)$(PAMDIR)/$(MODULE)"
	@echo "Configure PAM by editing /etc/pam.d/sudo (see pam.d/sudo.example)"

uninstall:
	@if [ -f /etc/NIXOS ] || [ -d /run/current-system/sw ]; then \
		echo "ERROR: NixOS detected – manual uninstall not supported. Remove from configuration.nix instead."; \
		exit 1; \
	fi
	rm -f $(DESTDIR)$(PAMDIR)/$(MODULE)
	@echo "Removed $(DESTDIR)$(PAMDIR)/$(MODULE)"

# Auto-generated header dependency tracking
-include $(DEP)
