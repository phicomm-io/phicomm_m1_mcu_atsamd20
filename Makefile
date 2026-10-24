HOST_CC ?= cc
CROSS_COMPILE ?= arm-none-eabi-
ARM_CC := $(CROSS_COMPILE)gcc
HAL_PYTHON ?= python3
export VERSION_TAG
HOST_FLAGS := -std=c11 -O2 -g -Wall -Wextra -Werror -Iinclude
ARM_FLAGS := -std=gnu11 -Os -g -mcpu=cortex-m0plus -mthumb -ffreestanding -fno-builtin -ffunction-sections -fdata-sections -fno-unwind-tables -fno-asynchronous-unwind-tables -Wall -Wextra -Werror -Iinclude -Iplatform -Ithird_party/samd20/include -Ithird_party/cmsis/include
SOURCES := src/core.c src/rtc.c src/display.c src/climate.c src/update.c src/runtime.c src/debug.c
PLATFORM := platform/startup.c platform/memory.c platform/samd20.c
VERSION_HEADER := include/m1/generated/build_version.h
.PHONY: all version FORCE test sanitize link-check firmware bootloader ota hal-check clean
all: test firmware bootloader
FORCE:
$(VERSION_HEADER): FORCE
	python3 tools/check_version.py --tag "$$VERSION_TAG"
version: $(VERSION_HEADER)
build:
	mkdir -p $@
build/test_core: src/core.c include/m1/core.h tests/test_core.c $(VERSION_HEADER) | build
	$(HOST_CC) $(HOST_FLAGS) src/core.c tests/test_core.c -o $@
build/test_runtime: $(SOURCES) tests/test_runtime.c $(wildcard include/m1/*.h) $(VERSION_HEADER) | build
	$(HOST_CC) $(HOST_FLAGS) $(SOURCES) tests/test_runtime.c -o $@
build/test_update: $(SOURCES) tests/test_update.c $(wildcard include/m1/*.h) $(VERSION_HEADER) | build
	$(HOST_CC) $(HOST_FLAGS) $(SOURCES) tests/test_update.c -o $@
test: build/test_core build/test_runtime build/test_update
	./build/test_core
	./build/test_runtime
	./build/test_update
	python3 -m unittest discover -s tests -p 'test_*.py' -v
sanitize: $(VERSION_HEADER) | build
	$(HOST_CC) $(HOST_FLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer $(SOURCES) tests/test_runtime.c -o build/test_runtime_sanitize
	./build/test_runtime_sanitize
	$(HOST_CC) $(HOST_FLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer $(SOURCES) tests/test_update.c -o build/test_update_sanitize
	./build/test_update_sanitize
link-check: firmware
firmware: $(VERSION_HEADER) | build
	$(ARM_CC) $(ARM_FLAGS) -nostdlib -Wl,--gc-sections,-T,platform/samd20g18a.ld,-Map,build/m1-app.map $(SOURCES) $(PLATFORM) platform/main.c -lgcc -o build/m1-app.elf
	$(CROSS_COMPILE)size build/m1-app.elf
	$(CROSS_COMPILE)objcopy -O binary build/m1-app.elf build/m1-app.bin
	python3 tools/check_elf.py build/m1-app.elf
	python3 tools/artifact_manifest.py build/m1-app.bin --compiler $(ARM_CC)
bootloader: $(VERSION_HEADER) | build
	$(ARM_CC) $(ARM_FLAGS) -DM1_BOOTLOADER -nostdlib -Wl,--gc-sections,-T,platform/boot.ld,-Map,build/m1-boot.map src/core.c src/update.c $(PLATFORM) platform/boot.c -lgcc -o build/m1-boot.elf
	$(CROSS_COMPILE)size build/m1-boot.elf
	$(CROSS_COMPILE)objcopy -O binary build/m1-boot.elf build/m1-boot.bin
	python3 tools/check_elf.py build/m1-boot.elf --boot
	python3 tools/artifact_manifest.py build/m1-boot.bin --compiler $(ARM_CC)
ota: firmware bootloader build/test_update
	python3 tools/pack_ota.py build/m1-app.bin --manifest build/m1-app.json
	./build/test_update build/ota/m1-mcu-ota.bin
hal-check: firmware bootloader
	$(HAL_PYTHON) tools/verify_platform_clock.py build/m1-app.elf build/m1-boot.elf --report build/platform-clock-verification.json
clean:
	rm -rf build
