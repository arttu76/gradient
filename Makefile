# Cross-compile gradient.c -> AmigaOS Hunk executable.
# Run `. ./env.sh` first to put the local vbcc toolchain on PATH.
# See BUILDING.md for the toolchain install.

CC      = vc
CFLAGS  = +aos68k -O1 -c99
LDLIBS  = -lamiga

all: Gradient

Gradient: gradient.c
	$(CC) $(CFLAGS) -o $@ gradient.c $(LDLIBS)

clean:
	rm -f Gradient *.o *.asm

.PHONY: all clean
