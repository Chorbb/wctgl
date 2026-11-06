# Сборка и проверки ядра порта. Требуется gcc или clang.
CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -Wno-array-bounds
CORE = core/cpu6502.c core/nes.c core/rt.c
BUILD = build

all: $(BUILD)/cpu_smoke $(BUILD)/replay $(BUILD)/boot $(BUILD)/api_test

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/cpu_smoke: tests/cpu_smoke.c core/cpu6502.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $^

$(BUILD)/replay: tests/replay.c $(CORE) core/*.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/replay.c $(CORE)

$(BUILD)/boot: tests/boot.c $(CORE) core/*.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/boot.c $(CORE)

$(BUILD)/api_test: tests/api_test.c core/tgl_api.c $(CORE) core/*.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/api_test.c core/tgl_api.c $(CORE)

# Библиотека ядра для Windows x64 (Godot). Нужны clang и lld-link; среда выполнения C не нужна.
WINFLAGS = --target=x86_64-pc-windows-msvc -O2 -DTGL_FREESTANDING -ffreestanding -fno-stack-protector -mno-stack-arg-probe -Wno-array-bounds
dll: | $(BUILD)
	mkdir -p $(BUILD)/win
	clang $(WINFLAGS) -c core/cpu6502.c -o $(BUILD)/win/cpu6502.o
	clang $(WINFLAGS) -c core/nes.c -o $(BUILD)/win/nes.o
	clang $(WINFLAGS) -c core/tgl_api.c -o $(BUILD)/win/tgl_api.o
	clang --target=x86_64-pc-windows-msvc -O1 -fno-builtin -DTGL_FREESTANDING -ffreestanding -c core/rt.c -o $(BUILD)/win/rt.o
	lld-link /dll /noentry /nodefaultlib /machine:x64 /out:godot/native/tglcore.dll $(BUILD)/win/*.o

# make test ROM=путь/к/игре.nes LOG=путь/к/run.bin
test: all
	$(BUILD)/cpu_smoke
	$(BUILD)/replay $(ROM) $(LOG)

clean:
	rm -rf $(BUILD)
