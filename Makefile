# Сборка и проверки ядра порта. Требуется gcc или clang.
CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -Wno-array-bounds
CORE = core/cpu6502.c core/nes.c core/rt.c
BUILD = build

all: $(BUILD)/cpu_smoke $(BUILD)/replay $(BUILD)/boot $(BUILD)/api_test $(BUILD)/xtest_base

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/cpu_smoke: tests/cpu_smoke.c core/cpu6502.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $^

$(BUILD)/replay: tests/replay.c $(CORE) core/*.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/replay.c $(CORE)

$(BUILD)/boot: tests/boot.c $(CORE) core/*.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/boot.c $(CORE)

# переведённый код, если он сгенерирован (make coverage gen), иначе ядро без перевода
XLAT_SRC = core/xlat.c $(if $(wildcard $(BUILD)/gen/xlat_gen.c),$(BUILD)/gen/xlat_gen.c,core/xlat_none.c)
XLAT_CFLAGS = -Wno-unused-label -Wno-misleading-indentation -Icore

$(BUILD)/api_test: tests/api_test.c core/tgl_api.c $(XLAT_SRC) $(CORE) core/*.h | $(BUILD)
	$(CC) $(CFLAGS) $(XLAT_CFLAGS) -o $@ tests/api_test.c core/tgl_api.c $(XLAT_SRC) $(CORE)

# Перевод: покрытие кода по записи → генерация C из ROM → проверка.
# make coverage gen xlat ROM=recordings/tgl.nes LOG=recordings/run1.bin FRAMES=3000
FRAMES ?= 3000
XLAT_ROUTINES ?= tools/routines.txt
$(BUILD)/xtest_base: tests/xtest.c tests/reclog.h core/xlat.c core/xlat_none.c $(CORE) core/*.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/xtest.c core/xlat.c core/xlat_none.c $(CORE)

# покрытие: обе записи, объединённо
# покрытие: старые записи от перезагрузки + отрезки от каждого снимка tgl_logger5 (recordings/l5/snap_*.bin)
SNAPS ?= $(wildcard recordings/l5/snap_*.bin)
coverage: $(BUILD)/xtest_base
	$(BUILD)/xtest_base coverage $(ROM) recordings/run1.bin 100000 $(BUILD)/cov1.txt
	$(BUILD)/xtest_base coverage $(ROM) recordings/run2.bin 100000 $(BUILD)/cov2.txt
	for s in $(SNAPS); do $(BUILD)/xtest_base coverage $(ROM) $$s 1200 $(BUILD)/cov_$$(basename $$s .bin).txt; done
	sort -u $(BUILD)/cov1.txt $(BUILD)/cov2.txt $(foreach s,$(SNAPS),$(BUILD)/cov_$(basename $(notdir $(s))).txt) > $(BUILD)/coverage.txt

gen:
	mkdir -p $(BUILD)/gen
	python3 tools/xlat.py $(ROM) $(BUILD)/coverage.txt $(XLAT_ROUTINES) $(BUILD)/gen/xlat_gen.c --closure --rts-entries

$(BUILD)/xtest: tests/xtest.c tests/reclog.h core/xlat.c $(BUILD)/gen/xlat_gen.c $(CORE) core/*.h
	$(CC) $(CFLAGS) -Wno-unused-label -Wno-misleading-indentation -Icore -o $@ tests/xtest.c core/xlat.c $(BUILD)/gen/xlat_gen.c $(CORE)

xlat: $(BUILD)/xtest
	$(BUILD)/xtest check $(ROM) $(LOG) $(FRAMES)
	$(BUILD)/xtest game $(ROM) $(LOG) $(FRAMES)

# Библиотека ядра для Windows x64 (Godot). Нужны clang и lld-link; среда выполнения C не нужна.
WINFLAGS = --target=x86_64-pc-windows-msvc -O2 -DTGL_FREESTANDING -ffreestanding -fno-stack-protector -mno-stack-arg-probe -Wno-array-bounds
dll: | $(BUILD)
	mkdir -p $(BUILD)/win
	clang $(WINFLAGS) -c core/cpu6502.c -o $(BUILD)/win/cpu6502.o
	clang $(WINFLAGS) -c core/nes.c -o $(BUILD)/win/nes.o
	clang $(WINFLAGS) -c core/tgl_api.c -o $(BUILD)/win/tgl_api.o
	clang $(WINFLAGS) -c core/xlat.c -o $(BUILD)/win/xlat.o
	clang $(WINFLAGS) -DXLAT_NOTRACE $(XLAT_CFLAGS) -c $(word 2,$(XLAT_SRC)) -o $(BUILD)/win/xlat_gen.o
	clang --target=x86_64-pc-windows-msvc -O1 -fno-builtin -DTGL_FREESTANDING -ffreestanding -c core/rt.c -o $(BUILD)/win/rt.o
	lld-link /dll /noentry /nodefaultlib /machine:x64 /out:godot/native/tglcore.dll $(BUILD)/win/*.o

# make test ROM=путь/к/игре.nes LOG=путь/к/run.bin
test: all
	$(BUILD)/cpu_smoke
	$(BUILD)/replay $(ROM) $(LOG)

clean:
	rm -rf $(BUILD)

.PHONY: all test clean dll coverage gen xlat
