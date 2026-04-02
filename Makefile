# pam_fprint_fixed – Makefile (stub, finalised in task 5)

CC       ?= gcc
CFLAGS   := -Wall -Wextra -Werror -fPIC -O2 \
            $(shell pkg-config --cflags dbus-1) \
            $(shell pkg-config --cflags pam)
LDFLAGS  := -shared \
            $(shell pkg-config --libs dbus-1)
# pam has no .pc on many distros; link explicitly
LDFLAGS  += -lpam

SRC      := src/pam_fprint_fixed.c src/fprintd_dbus.c
OBJ      := $(SRC:.c=.o)
TARGET   := pam_fprint_fixed.so

.PHONY: all clean install

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJ) $(TARGET)

install:
	@echo "Run install.sh as root instead"
