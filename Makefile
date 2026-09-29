CLANG_FORMAT ?= clang-format
HOST_CC ?= cc
STYLE_FILES := $(wildcard main/*.c main/*.h tests/*.c)
HOST_TEST := build/host/click_protocol_test
HOST_FLAGS := -std=c11 -Wall -Wextra -Werror -Wconversion -Wshadow \
              -Wstrict-prototypes -g -fsanitize=address,undefined

.PHONY: check format check-format test

check: check-format test

format:
	$(CLANG_FORMAT) -i $(STYLE_FILES)

check-format:
	$(CLANG_FORMAT) --dry-run --Werror $(STYLE_FILES)

test: $(HOST_TEST)
	./$(HOST_TEST)

$(HOST_TEST): main/click_protocol.c main/click_protocol.h tests/click_protocol_test.c Makefile
	mkdir -p build/host
	$(HOST_CC) $(HOST_FLAGS) -Imain main/click_protocol.c tests/click_protocol_test.c -o $@
