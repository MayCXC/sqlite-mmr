CC ?= gcc
PREFIX ?= /usr/local
INSTALL_LIB_DIR = $(PREFIX)/lib
INSTALL_INCLUDE_DIR = $(PREFIX)/include

CFLAGS += -Wall -Wextra

TARGET_LOADABLE = mmr0.so

ifeq ($(shell uname),Darwin)
  TARGET_LOADABLE = mmr0.dylib
endif

$(TARGET_LOADABLE): mmr0.c mmr0.h
	$(CC) -fPIC -shared -O2 $(CFLAGS) $< -o $@

static: mmr0.c mmr0.h
	$(CC) -c -O2 -DSQLITE_CORE -DSQLITE_MMR_STATIC $(CFLAGS) $< -o mmr0.o

install: $(TARGET_LOADABLE) mmr0.h
	install -d $(INSTALL_LIB_DIR)
	install -d $(INSTALL_INCLUDE_DIR)
	install -m 644 $(TARGET_LOADABLE) $(INSTALL_LIB_DIR)
	install -m 644 mmr0.h $(INSTALL_INCLUDE_DIR)

test: $(TARGET_LOADABLE)
	sqlite3 :memory: '.load ./mmr0' '.read tests/test_basic.sql'

clean:
	rm -f $(TARGET_LOADABLE) mmr0.o

.PHONY: static install test clean
