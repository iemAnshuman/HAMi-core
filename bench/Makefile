UNAME_S := $(shell uname -s)
BUILD_DIR ?= build
CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror
LDLIBS :=

ifeq ($(UNAME_S),Linux)
LDLIBS += -ldl
endif

.PHONY: all smoke clean

all: $(BUILD_DIR)/cuinit_bench

$(BUILD_DIR)/cuinit_bench: cuinit_bench.c
	mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $< $(LDLIBS) -o $@

smoke: all
	bash test/smoke.sh

clean:
	rm -rf $(BUILD_DIR)
