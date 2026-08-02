APP_NAME := geistshell

DEPS_DIR  := deps
GEIST_DIR := $(DEPS_DIR)/geist
# Official upstream engine. Pin GEIST_REF to a commit/tag for reproducible
# builds; override either on the command line to track a fork or branch.
GEIST_REPO ?= https://github.com/geisten/geistlib.git
GEIST_REF  ?= v0.2.1

BUILD_MODE ?= host-debug

# Optional REMOTE model adapter (libcurl transport, OpenAI-compatible). Off by
# default so the standard build needs no libcurl; enable with `make REMOTE=1`.
REMOTE ?= 0
ifeq ($(REMOTE),1)
    REMOTE_DEFS := -DSPG_ENABLE_REMOTE
    REMOTE_LIBS := -lcurl
endif

HOST_CC ?= clang

AR ?= ar
LIBOMP_PREFIX ?= /opt/homebrew/opt/libomp

ifeq ($(BUILD_MODE),host-debug)
    MODE_DIR := host-debug
    CC := $(HOST_CC)
    SPG_OPT_FLAGS := -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
    SPG_LD_FLAGS := -fsanitize=address,undefined
    GEIST_MODE := asan
    GEIST_TARGET := $(shell if [ -x "$(GEIST_DIR)/mk/detect-target.sh" ]; then cd "$(GEIST_DIR)" && mk/detect-target.sh; else echo mac; fi)
else ifeq ($(BUILD_MODE),host-release)
    MODE_DIR := host-release
    CC := $(HOST_CC)
    SPG_OPT_FLAGS := -O3 -DNDEBUG
    SPG_LD_FLAGS :=
    GEIST_MODE := release
    GEIST_TARGET := $(shell if [ -x "$(GEIST_DIR)/mk/detect-target.sh" ]; then cd "$(GEIST_DIR)" && mk/detect-target.sh; else echo mac; fi)
else
    $(error Unknown BUILD_MODE=$(BUILD_MODE). Use host-debug or host-release)
endif

BUILD_DIR := build/$(MODE_DIR)
OBJ_DIR   := $(BUILD_DIR)/obj
BIN_DIR   := $(BUILD_DIR)/bin
LIB_DIR   := $(BUILD_DIR)/lib
TEST_DIR  := $(BUILD_DIR)/test

SPG_LIB := $(LIB_DIR)/libgeistshell.a
SPG_BIN := $(BIN_DIR)/$(APP_NAME)
CHAT_BIN := $(BIN_DIR)/geistshell-chat

GEIST_LIB := $(GEIST_DIR)/lib/$(GEIST_TARGET)/$(GEIST_MODE)/libgeist.a

ifeq ($(GEIST_TARGET),mac-omp)
    GEIST_LINK_FLAGS := -framework Accelerate -L$(LIBOMP_PREFIX)/lib -Wl,-rpath,$(LIBOMP_PREFIX)/lib -lomp
else ifeq ($(GEIST_TARGET),mac)
    GEIST_LINK_FLAGS := -framework Accelerate
else ifeq ($(GEIST_TARGET),pi5)
    GEIST_LINK_FLAGS := -fopenmp -lopenblas -lfftw3f
else
    GEIST_LINK_FLAGS :=
endif

CPPFLAGS := -Iinclude -Iinclude/geistshell -I$(GEIST_DIR)/include -I$(GEIST_DIR) -I$(DEPS_DIR)/jsmn
WARNINGS := -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wstrict-prototypes
CFLAGS := -std=c23 $(WARNINGS) $(SPG_OPT_FLAGS) $(CPPFLAGS) $(REMOTE_DEFS)
LDLIBS := $(GEIST_LINK_FLAGS) -lm -lpthread $(REMOTE_LIBS)


SPG_SOURCES := \
    src/actor/actor.c \
    src/actor/recommendation.c \
    src/chat/chat_template.c \
    src/chat/chat_tools.c \
    src/core/allocator.c \
    src/core/budget_config.c \
    src/core/hash.c \
    src/core/hmac.c \
    src/core/run_config.c \
    src/core/status.c \
    src/context/context.c \
    src/dsl/schema.c \
    src/dsl/sexpr.c \
    src/eval/eval.c \
    src/eval/guard_ring.c \
    src/exec/cmd_executor.c \
    src/exec/cmd_registry.c \
    src/exec/exec_command.c \
    src/exec/host_probe.c \
    src/exec/shell_executor.c \
    src/improve/improve.c \
    src/executor/executor_boundary.c \
    src/graph/graph.c \
    src/journal/journal.c \
    src/journal/journal_sign.c \
    src/memory/mem_command.c \
    src/memory/mem_executor.c \
    src/memory/mem_store.c \
    src/memory/memory.c \
    src/model/grammar_mask.c \
    src/model/model_adapter.c \
    src/model/model_remote_codec.c \
    src/model/model_resolve.c \
    src/policy/policy.c \
    src/policy/policy_config.c \
    src/policy/policy_gate.c \
    src/run/agent_loop.c \
    src/run/agent_run.c \
    src/run/orchestrator.c \
    src/sim/risk.c \
    src/sim/sim_executor.c \
    src/sim/sim_config.c

ifeq ($(REMOTE),1)
    SPG_SOURCES += src/model/model_remote.c
endif

CLI_SOURCES := src/cli/main.c
CHAT_SOURCES := src/chat/main.c
TEST_SOURCES := $(wildcard test/test_*.c)
CLI_TESTS := $(wildcard test/test_cli_*.sh)

SPG_OBJECTS := $(patsubst %.c,$(OBJ_DIR)/%.o,$(SPG_SOURCES))
CLI_OBJECTS := $(patsubst %.c,$(OBJ_DIR)/%.o,$(CLI_SOURCES))
CHAT_OBJECTS := $(patsubst %.c,$(OBJ_DIR)/%.o,$(CHAT_SOURCES))
TEST_BINS := $(patsubst test/%.c,$(TEST_DIR)/%,$(TEST_SOURCES))
DEPS := $(SPG_OBJECTS:.o=.d) $(CLI_OBJECTS:.o=.d) $(CHAT_OBJECTS:.o=.d)

.PHONY: all build-mode host-debug host-release sync-engine update-engine lib test clean distclean help

all: host-debug

build-mode: $(SPG_BIN) $(CHAT_BIN)

host-debug:
	$(MAKE) BUILD_MODE=host-debug build-mode

host-release:
	$(MAKE) BUILD_MODE=host-release build-mode

sync-engine:
	@mkdir -p $(DEPS_DIR)
	@if [ ! -d "$(GEIST_DIR)/.git" ]; then \
		echo "Cloning libgeist from $(GEIST_REPO) @ $(GEIST_REF)"; \
		rm -rf "$(GEIST_DIR)"; \
		git clone --quiet "$(GEIST_REPO)" "$(GEIST_DIR)"; \
		git -C "$(GEIST_DIR)" checkout --quiet $(GEIST_REF); \
	else \
		echo "libgeist already present at $(GEIST_DIR)"; \
	fi

update-engine:
	@mkdir -p $(DEPS_DIR)
	@if [ ! -d "$(GEIST_DIR)/.git" ]; then \
		$(MAKE) sync-engine; \
	else \
		echo "Updating libgeist from $(GEIST_REPO) @ $(GEIST_REF)"; \
		git -C "$(GEIST_DIR)" fetch --quiet --tags origin; \
		git -C "$(GEIST_DIR)" checkout --quiet $(GEIST_REF); \
	fi

$(GEIST_LIB): sync-engine
	$(MAKE) -C $(GEIST_DIR) TARGET=$(GEIST_TARGET) MODE=$(GEIST_MODE) lib

lib: $(SPG_LIB)

$(SPG_LIB): $(SPG_OBJECTS)
	@mkdir -p $(@D)
	$(AR) rcs $@ $^

$(SPG_BIN): $(CLI_OBJECTS) $(SPG_LIB) $(GEIST_LIB)
	@mkdir -p $(@D)
	$(CC) $(SPG_LD_FLAGS) -o $@ $(CLI_OBJECTS) $(SPG_LIB) $(GEIST_LIB) $(LDLIBS)

$(CHAT_BIN): $(CHAT_OBJECTS) $(SPG_LIB) $(GEIST_LIB)
	@mkdir -p $(@D)
	$(CC) $(SPG_LD_FLAGS) -o $@ $(CHAT_OBJECTS) $(SPG_LIB) $(GEIST_LIB) $(LDLIBS)

$(OBJ_DIR)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

test: $(TEST_BINS) $(SPG_BIN)
	@status=0; \
	for t in $(TEST_BINS); do \
		echo "$$t"; \
		"$$t" || status=$$?; \
	done; \
	for t in $(CLI_TESTS); do \
		echo "$$t"; \
		SPG_BIN="$(SPG_BIN)" sh "$$t" || status=$$?; \
	done; \
	exit $$status

$(TEST_DIR)/%: test/%.c $(SPG_LIB) $(GEIST_LIB)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(SPG_LD_FLAGS) -o $@ $< $(SPG_LIB) $(GEIST_LIB) $(LDLIBS)

clean:
	rm -rf build/host-debug build/host-release dist

distclean: clean
	rm -rf $(DEPS_DIR)

help:
	@echo "geistshell build"
	@echo "  make                 build host-debug"
	@echo "  make host-debug      build ASan/UBSan host binary"
	@echo "  make host-release    build optimized host binary"
	@echo "  make test            build and run standalone tests"
	@echo "  make REMOTE=1 ...     build with the libcurl remote model adapter"
	@echo "  make sync-engine     clone deps/geist from GitHub if missing"
	@echo "  make update-engine   checkout the pinned GEIST_REF in deps/geist"
	@echo "  make clean           remove top-level build outputs"
	@echo "  make distclean       remove build outputs and deps"

-include $(DEPS)
