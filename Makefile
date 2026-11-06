# Сборка и проверки ядра порта. Требуется gcc или clang.
CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -Wno-array-bounds
CORE = core/cpu6502.c core/nes.c
BUILD = build

all: $(BUILD)/cpu_smoke $(BUILD)/replay $(BUILD)/boot

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/cpu_smoke: tests/cpu_smoke.c core/cpu6502.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $^

$(BUILD)/replay: tests/replay.c $(CORE) core/*.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/replay.c $(CORE)

$(BUILD)/boot: tests/boot.c $(CORE) core/*.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/boot.c $(CORE)

# make test ROM=путь/к/игре.nes LOG=путь/к/run.bin
test: all
	$(BUILD)/cpu_smoke
	$(BUILD)/replay $(ROM) $(LOG)

clean:
	rm -rf $(BUILD)
