CC = cc
CPPFLAGS += -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 -Isrc
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic
LDLIBS += -lz -lcrypto
SOURCES = src/main.c src/utils.c src/linux.c src/scanner.c src/preview.c src/stream.c src/metadata.c src/json.c src/backup.c src/verify.c
OBJECTS = $(SOURCES:.c=.o)

all: vinylscan
vinylscan: $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS) $(LDLIBS)

$(OBJECTS): src/vinylscan.h

docs:
	sh tests/docs.sh

test: docs vinylscan
	sh tests/test.sh

clean:
	rm -f vinylscan $(OBJECTS) tests/fault.so tests/unit

.PHONY: all docs test clean
