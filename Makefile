# gb28181-ipc-lab — 构建脚本
# ============================================================
# 目标：
#   make            本机编译（gcc）      -> build/gb28181_device
#   make aarch64    交叉编译（静态链接） -> build/gb28181_device_aarch64
#   make test       编译并运行 tests/ 下的单元测试
#   make run        用 config.ini 跑本机版本
#   make clean      清掉 build/
#
# 为什么 aarch64 要 -static：
#   目标环境（开发板 / qemu 用户态模拟）不一定有相同的 libc 版本，
#   静态链接直接免掉整套库依赖 —— 现场只关心「能不能跑」，没空查「缺哪个 .so」。
#
# 为什么必须保持 -Wall -Wextra 零警告：
#   这套代码是拿来当范例讲的，一个警告就说明有一处「能跑但语义可疑」的地方。
#   协议代码里最贵的 bug（比如 PTS 错位一位）恰恰长得不像 bug。

CC         ?= gcc
CROSS      ?= aarch64-linux-gnu-gcc

CFLAGS     ?= -Wall -Wextra -O2 -pthread
CROSSFLAGS ?= -static -Wall -Wextra -O2 -pthread

SRC_DIR := src
BUILD   := build
BIN     := $(BUILD)/gb28181_device
BIN_ARM := $(BUILD)/gb28181_device_aarch64
OBJDIR  := $(BUILD)/obj

SRCS     := $(wildcard $(SRC_DIR)/*.c)
HEADERS  := $(wildcard $(SRC_DIR)/*.h)
# 单元测试要链所有模块，但不能链 main.c（测试自己提供 main）
LIB_SRCS := $(filter-out $(SRC_DIR)/main.c,$(SRCS))
OBJS     := $(patsubst $(SRC_DIR)/%.c,$(OBJDIR)/%.o,$(SRCS))
TEST_SRCS := $(wildcard tests/*.c)

.PHONY: all aarch64 test run clean

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^
	@echo "[build] 完成：$@"

$(OBJDIR)/%.o: $(SRC_DIR)/%.c $(HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

aarch64: $(BIN_ARM)

$(BIN_ARM): $(SRCS) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CROSS) $(CROSSFLAGS) -o $@ $(SRCS)
	@file $@ || true

# 单元测试：tests/ 下每个 .c 都是「自带 main 的独立程序」，逐个编译 + 逐个运行
# （不能一把链在一起，否则多个 main 冲突）。
#   命名约定：test_*.c 是用例，跑起来必须返回 0；
#             其它 .c（如 mkps_pts）是辅助工具，只编译不当作用例运行。
TEST_SRCS   := $(wildcard tests/test_*.c)
TEST_BINS   := $(patsubst tests/%.c,$(BUILD)/tests/%,$(TEST_SRCS))
HELPER_SRCS := $(filter-out $(TEST_SRCS),$(wildcard tests/*.c))
HELPER_BINS := $(patsubst tests/%.c,$(BUILD)/tests/%,$(HELPER_SRCS))

test: $(HELPER_BINS)
	@if [ -z "$(TEST_SRCS)" ]; then \
		echo "[test] tests/*.c 还没就绪，跳过（测试文件补上后 make test 会自动编译并运行）"; \
	else \
		$(MAKE) --no-print-directory run-tests; \
	fi

run-tests: $(TEST_BINS)
	@fail=0; \
	for t in $(TEST_BINS); do \
		echo "==== $$t ===="; \
		$$t || fail=1; \
	done; \
	if [ $$fail -ne 0 ]; then echo "[test] 有失败用例"; exit 1; fi; \
	echo "[test] 全部用例通过（$(words $(TEST_BINS)) 个）"

$(BUILD)/tests/%: tests/%.c $(LIB_SRCS) $(HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I$(SRC_DIR) -o $@ $< $(LIB_SRCS)

run: $(BIN)
	$(BIN) -c config.ini

clean:
	rm -rf $(BUILD)
	@echo "[clean] 已删除 $(BUILD)/"
