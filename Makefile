CFLAGS  := -std=c99 -Wall -O2 -D_REENTRANT
LIBS    := -lpthread -lm -lcrypto -lssl

TARGET  := $(shell uname -s | tr '[A-Z]' '[a-z]' 2>/dev/null || echo unknown)

ifeq ($(TARGET), sunos)
	CFLAGS += -D_PTHREADS -D_POSIX_C_SOURCE=200112L
	LIBS   += -lsocket
else ifeq ($(TARGET), darwin)
	# Per https://luajit.org/install.html: If MACOSX_DEPLOYMENT_TARGET
	# is not set then it's forced to 10.4, which breaks compile on Mojave.
	export MACOSX_DEPLOYMENT_TARGET = $(shell sw_vers -productVersion)

	OPENSSL_PREFIX := $(shell brew --prefix openssl 2>/dev/null || echo /usr/local/opt/openssl)
	LIBS   += -L$(OPENSSL_PREFIX)/lib
	CFLAGS += -I$(OPENSSL_PREFIX)/include
else ifeq ($(TARGET), linux)
        CFLAGS  += -D_GNU_SOURCE
	LIBS    += -ldl
	LDFLAGS += -Wl,-E
else ifeq ($(TARGET), freebsd)
	CFLAGS  += -D_DECLARE_C99_LDBL_MATH
	LDFLAGS += -Wl,-E
endif

SRC  := wrk.c net.c ssl.c aprintf.c stats.c script.c units.c \
		ae.c zmalloc.c http_parser.c tinymt64.c hdr_histogram.c
BIN  := wrk

ODIR := obj
OBJ  := $(patsubst %.c,$(ODIR)/%.o,$(SRC)) $(ODIR)/bytecode.o

LDIR     = deps/luajit/src
LIBS    := -lluajit $(LIBS)
CFLAGS  += -I$(LDIR)
LDFLAGS += -L$(LDIR)

all: $(BIN)

clean:
	$(RM) $(BIN) obj/*
	@$(MAKE) -C deps/luajit clean

$(BIN): $(OBJ)
	@echo LINK $(BIN)
	@$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

$(OBJ): config.h Makefile $(LDIR)/libluajit.a | $(ODIR)

$(ODIR):
	@mkdir -p $@

$(ODIR)/bytecode.o: src/wrk.lua
	@echo LUAJIT $<
	@$(SHELL) -c 'cd $(LDIR) && ./luajit -b $(CURDIR)/$< $(CURDIR)/$@'

$(ODIR)/%.o : %.c
	@echo CC $<
	@$(CC) $(CFLAGS) -c -o $@ $<

$(LDIR)/libluajit.a:
	@echo Building LuaJIT...
	@$(MAKE) -C $(LDIR) BUILDMODE=static

test: test-unit test-e2e
UNITY_SRC := deps/unity/unity.c
UNITY_INC := -Ideps/unity
TEST_UNIT_SRC := tests/unit/runner.c
TEST_UNIT_BIN := obj/test_unit

TEST_STATS_SRC := tests/unit/test_stats.c
TEST_STATS_BIN := obj/test_stats
STATS_DEPS     := src/stats.c src/zmalloc.c src/tinymt64.c src/hdr_histogram.c

TEST_UNITS_SRC := tests/unit/test_units.c
TEST_UNITS_BIN := obj/test_units
UNITS_DEPS     := src/units.c src/aprintf.c src/zmalloc.c

TEST_HDR_SRC := tests/unit/test_hdr.c
TEST_HDR_BIN := obj/test_hdr
HDR_DEPS     := src/hdr_histogram.c

test-unit: $(TEST_UNIT_BIN) $(TEST_STATS_BIN) $(TEST_UNITS_BIN) $(TEST_HDR_BIN)
	@./$(TEST_UNIT_BIN)
	@./$(TEST_STATS_BIN)
	@./$(TEST_UNITS_BIN)
	@./$(TEST_HDR_BIN)

$(TEST_UNIT_BIN): $(TEST_UNIT_SRC) $(UNITY_SRC) | $(ODIR)
	@$(CC) $(CFLAGS) $(UNITY_INC) -o $@ $^

$(TEST_STATS_BIN): $(TEST_STATS_SRC) $(UNITY_SRC) $(STATS_DEPS) | $(ODIR)
	@$(CC) $(CFLAGS) $(UNITY_INC) -Isrc -DUNITY_INCLUDE_DOUBLE -o $@ $^ -lm -lpthread

$(TEST_UNITS_BIN): $(TEST_UNITS_SRC) $(UNITY_SRC) $(UNITS_DEPS) | $(ODIR)
	@$(CC) $(CFLAGS) $(UNITY_INC) -Isrc -include tests/unit/platform_compat.h -o $@ $^ -lpthread

$(TEST_HDR_BIN): $(TEST_HDR_SRC) $(UNITY_SRC) $(HDR_DEPS) | $(ODIR)
	@$(CC) $(CFLAGS) $(UNITY_INC) -Isrc -o $@ $^ -lm
test-e2e:
	@bash tests/e2e/smoke.sh
test-asan:
	@echo "no asan tests yet" && exit 0

.PHONY: all clean test test-unit test-e2e test-asan
.SUFFIXES:
.SUFFIXES: .c .o .lua

vpath %.c   src
vpath %.h   src
vpath %.lua scripts
