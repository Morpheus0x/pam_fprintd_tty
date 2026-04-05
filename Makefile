# pam_fprint_fixed – Build a shared PAM module (.so)
#
# Dependencies:
#   - libpam development headers  (pam package)
#   - libdbus-1 development headers (dbus package)
#   - pkg-config
#
# Usage:
#   make            – build pam_fprint_fixed.so
#   make clean      – remove build artefacts
#   make debug      – build with debug symbols, no optimisation
#   make install    – install .so + config (runs install.sh, needs root)

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
SRC      := $(SRCDIR)/pam_fprint_fixed.c $(SRCDIR)/fprintd_dbus.c
OBJ      := $(patsubst $(SRCDIR)/%.c,$(BUILDDIR)/%.o,$(SRC))
DEP      := $(OBJ:.o=.d)
TARGET   := $(BUILDDIR)/pam_fprint_fixed.so

# ── Targets ───────────────────────────────────────────────────────────

.PHONY: all clean debug install

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
	@./install.sh

# Auto-generated header dependency tracking
-include $(DEP)
