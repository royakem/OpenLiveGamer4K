# Offline build + tests for the openlivegamer4k-control app.
#
# `make` builds the GTK4 control binary (needs libgtk-4-dev).
# `make test` builds and runs the offline unit tests only — no GTK,
# no display, no device required. This is the CI/dev loop.

CC      ?= cc
CFLAGS  += -O2 -Wall -Wextra -std=gnu11 -Iinclude
PKG     ?= pkg-config

LIBS_BACKEND = src/olg4k-backend.c src/olg4k-backend-real.c \
               src/olg4k-backend-mock.c src/convert.c
LIBS_WORKER  = $(LIBS_BACKEND) src/olg4k-worker.c

BIN   = openlivegamer4k-control
TESTS = tests/test-backend-contract tests/test-convert tests/test-worker

.PHONY: all test clean
all: $(BIN)

$(BIN): src/main.c $(LIBS_WORKER)
	$(CC) $(CFLAGS) $(shell $(PKG) --cflags gtk4 json-glib-1.0) \
	    src/main.c $(LIBS_WORKER) \
	    -o $@ $(shell $(PKG) --libs gtk4 json-glib-1.0)

# ---- offline tests (no GTK) ----
test: $(TESTS)
	./tests/test-backend-contract
	./tests/test-convert
	./tests/test-worker

tests/test-backend-contract: tests/test-backend-contract.c $(LIBS_BACKEND)
	$(CC) $(CFLAGS) tests/test-backend-contract.c $(LIBS_BACKEND) -o $@

tests/test-convert: tests/test-convert.c src/convert.c
	$(CC) $(CFLAGS) tests/test-convert.c src/convert.c -o $@

tests/test-worker: tests/test-worker.c $(LIBS_WORKER)
	$(CC) $(CFLAGS) tests/test-worker.c $(LIBS_WORKER) -o $@

clean:
	rm -f $(BIN) $(TESTS) *.o
