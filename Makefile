HOST_CC ?= cc
CROSS_COMPILE ?= arm-none-eabi-
ARM_CC := $(CROSS_COMPILE)gcc
HOST_FLAGS := -std=c11 -O2 -g -Wall -Wextra -Werror -Iinclude
ARM_FLAGS := -std=gnu11 -Os -g -mcpu=cortex-m0plus -mthumb -ffreestanding -fno-builtin -fno-unwind-tables -fno-asynchronous-unwind-tables -Wall -Wextra -Werror -Iinclude

.PHONY: all test sanitize link-check clean
all: test
build:
	mkdir -p $@
build/test_core: src/core.c include/m1/core.h tests/test_core.c | build
	$(HOST_CC) $(HOST_FLAGS) src/core.c tests/test_core.c -o $@
test: build/test_core
	./build/test_core
	python3 -m unittest discover -s tests -p 'test_*.py' -v
sanitize: | build
	$(HOST_CC) $(HOST_FLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer src/core.c tests/test_core.c -o build/test_core_sanitize
	./build/test_core_sanitize
link-check: | build
	$(ARM_CC) $(ARM_FLAGS) -nostdlib -Wl,-T,platform/samd20g18a.ld,-Map,build/core-link-check.map src/core.c platform/startup.c platform/link_check.c platform/memory.c -lgcc -o build/core-link-check.elf
	$(CROSS_COMPILE)size build/core-link-check.elf
	python3 tools/check_elf.py build/core-link-check.elf
clean:
	rm -rf build
