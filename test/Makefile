CC = gcc
CFLAGS = -std=gnu2x -Wall -Wextra -Wpedantic -O1 -g -I../include
LDFLAGS = -ljson-c -lpthread

SRC_VALIDATION = validation-test.c ../lib/validation.c ../lib/json_util.c ../lib/thread_error.c
BIN_VALIDATION = test_validation
ASAN_BIN_VALIDATION = test_validation_asan
TSAN_BIN_VALIDATION = test_validation_tsan

STRICT_CFLAGS = -std=gnu2x -Wall -Wextra -Wpadded -Wdangling-pointer=2 -Wreturn-local-addr -Wpedantic -Wshadow -Werror -Wformat=2 -Wvla -Wimplicit-fallthrough -Wnull-dereference -Wundef -g -flto=auto -O3 -D_GNU_SOURCE -D_FORTIFY_SOURCE=3 -fstack-protector-strong -fstack-clash-protection -fcf-protection=full -march=x86-64-v3 -fPIE -I../include

.PHONY: all release asan tsan coverage clean bench

all: release asan tsan benchmark_strcopy

release: $(BIN_VALIDATION)

asan: $(ASAN_BIN_VALIDATION)

tsan: $(TSAN_BIN_VALIDATION)

benchmark_strcopy: benchmark_strcopy.c
	$(CC) $(STRICT_CFLAGS) $< -o $@

bench: benchmark_strcopy
	./benchmark_strcopy

coverage: $(BIN_VALIDATION)
	./$(BIN_VALIDATION)
	gcov-14 test_validation-validation.gcda

$(BIN_VALIDATION): $(SRC_VALIDATION)
	$(CC) $(CFLAGS) --coverage $^ -o $@ $(LDFLAGS) --coverage

$(ASAN_BIN_VALIDATION): $(SRC_VALIDATION)
	$(CC) $(CFLAGS) -fsanitize=address,leak $^ -o $@ $(LDFLAGS)

$(TSAN_BIN_VALIDATION): $(SRC_VALIDATION)
	sudo sysctl vm.mmap_rnd_bits=30
	$(CC) $(CFLAGS) -fsanitize=thread $^ -o $@ $(LDFLAGS)

clean:
	rm -f $(BIN_VALIDATION) $(ASAN_BIN_VALIDATION) $(TSAN_BIN_VALIDATION) benchmark_strcopy *.gcda *.gcno *.gcov ../src/*.gcda ../src/*.gcno ../lib/*.gcda ../lib/*.gcno
