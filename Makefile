CC ?= gcc
PREFIX ?= /usr/local
INSTALL_LIB_DIR = $(PREFIX)/lib
INSTALL_INCLUDE_DIR = $(PREFIX)/include

CFLAGS += -Wall -Wextra

TARGET_LOADABLE = mmr0.so

ifeq ($(shell uname),Darwin)
  TARGET_LOADABLE = mmr0.dylib
endif

$(TARGET_LOADABLE): sqlite_mmr.c sqlite_mmr.h
	$(CC) -fPIC -shared -O2 $(CFLAGS) $< -o $@

static: sqlite_mmr.c sqlite_mmr.h
	$(CC) -c -O2 -DSQLITE_CORE -DSQLITE_MMR_STATIC $(CFLAGS) $< -o sqlite_mmr.o

install: $(TARGET_LOADABLE) sqlite_mmr.h
	install -d $(INSTALL_LIB_DIR)
	install -d $(INSTALL_INCLUDE_DIR)
	install -m 644 $(TARGET_LOADABLE) $(INSTALL_LIB_DIR)
	install -m 644 sqlite_mmr.h $(INSTALL_INCLUDE_DIR)

test: $(TARGET_LOADABLE)
	sqlite3 :memory: '.load ./mmr0' '.read tests/test_basic.sql'

clean:
	rm -f $(TARGET_LOADABLE) sqlite_mmr.o

.PHONY: static install test clean
