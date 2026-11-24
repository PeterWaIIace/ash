CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -D_XOPEN_SOURCE=700 -g -Isrc
LDFLAGS =
LDLIBS =
DEPFLAGS = -MMD -MP
FRAMAC ?= frama-c
VERIFY_FCTS = flat_skip_marker,flat_skip_single,flat_skip_double,flat_balanced,flat_skip
SRC = $(wildcard src/*.c)
OBJ = $(patsubst src/%.c,build/%.o,$(SRC))
DEP = $(OBJ:.o=.d)

all: ash

ash: $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)

build/%.o: src/%.c
	mkdir -p build
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

build:
	$(MAKE) clean
	$(MAKE) all

test: ash
	bash tests/run.sh

verify:
	$(FRAMAC) -wp -wp-rte -wp-fct $(VERIFY_FCTS) -wp-prover alt-ergo \
		-cpp-extra-args=-D_XOPEN_SOURCE=700 src/expand.c

clean:
	rm -rf build ash

-include $(DEP)

.PHONY: all build test verify clean
