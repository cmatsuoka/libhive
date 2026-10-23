# Makefile for the Hive-V library (h5).
#
# Targets:
#   all     build libhive.a
#   check   build and run the test harness
#   clean   remove the build products

CC      ?= cc
AR      ?= ar
RANLIB  ?= ranlib
OPT     ?= -O2

CSTD     := -std=c99
WARN     := -Wall -Wextra -Werror
INCLUDES := -Iinclude -Isrc
CFLAGS   ?=

# The library must not use floating point anywhere. Ask the compiler to prove
# it where the warnings are available.
FLOAT_WARN := $(shell $(CC) -Wfloat-conversion -Wdouble-promotion -Werror \
	-E -x c /dev/null >/dev/null 2>&1 && echo "-Wfloat-conversion -Wdouble-promotion")

# The library is plain C with no exceptions or backtraces. Drop the unwind
# tables unless the compiler does not know the flags.
NO_UNWIND := $(shell $(CC) -fno-asynchronous-unwind-tables -fno-unwind-tables \
	-Werror -E -x c /dev/null >/dev/null 2>&1 && \
	echo "-fno-asynchronous-unwind-tables -fno-unwind-tables")

LIB_CFLAGS  := $(CSTD) $(WARN) $(OPT) $(FLOAT_WARN) $(NO_UNWIND) $(INCLUDES) $(CFLAGS) -MMD -MP
TEST_CFLAGS := $(CSTD) $(WARN) $(OPT) $(NO_UNWIND) $(INCLUDES) $(CFLAGS) -MMD -MP

BUILD := build

LIB_SRCS  := $(wildcard src/*.c)
TEST_SRCS := $(wildcard test/*.c)
LIB_OBJS  := $(patsubst src/%.c,$(BUILD)/src/%.o,$(LIB_SRCS))
TEST_OBJS := $(patsubst test/%.c,$(BUILD)/test/%.o,$(TEST_SRCS))

LIB  := libhive.a
TEST := h5_test

.PHONY: all check clean

all: $(LIB)

$(LIB): $(LIB_OBJS)
	$(AR) rcs $@ $^
	$(RANLIB) $@

$(TEST): $(TEST_OBJS) $(LIB)
	$(CC) $(TEST_CFLAGS) -o $@ $(TEST_OBJS) -L. -lhive -lm

$(BUILD)/src/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(LIB_CFLAGS) -c -o $@ $<

$(BUILD)/test/%.o: test/%.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_CFLAGS) -c -o $@ $<

check: $(TEST)
	@mkdir -p $(BUILD)
	./$(TEST) --init
	./$(TEST) --sine
	./$(TEST) --dsp
	./$(TEST) --silence $(BUILD)/silence.wav
	./$(TEST) --api
	./$(TEST) --tom
	./$(TEST) --snare

clean:
	rm -rf $(BUILD) $(LIB) $(TEST)

-include $(LIB_OBJS:.o=.d) $(TEST_OBJS:.o=.d)
