# cnet-cli -- CNet BBS standalone admin CLI
#
# Cross-compiled with m68k-amigaos-gcc for AmigaOS 3.x (68020+).
# Links against cnet.library at runtime via inline stubs.

CC       = /opt/amiga/bin/m68k-amigaos-gcc

# CNET_SDK_PATH can be overridden on the command line or via environment.
# Default: co-located checkout at ../cnet-sdk
CNET_SDK_PATH ?= ../cnet-sdk

CFLAGS   = -noixemul -m68020 -O2 -Wall -Wextra -Werror
CFLAGS  += -I$(CNET_SDK_PATH)/include

LDFLAGS  = -noixemul

TARGET   = cnet-cli
SRCS     = $(wildcard src/*.c)
OBJS     = $(SRCS:.c=.o)

.PHONY: all clean

all: $(TARGET)

# Rebuild all objects if any header changes
$(OBJS): $(wildcard src/*.h)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f src/*.o $(TARGET)
