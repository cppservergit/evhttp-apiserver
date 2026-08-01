CC = gcc
CFLAGS = -std=gnu2x -Wall -Wextra -Wpedantic -O1 -g -I../include
LDFLAGS = -ljson-c -lpthread

SRC_VALIDATION = validation-test.c ../src/validation.c ../src/json_util.c ../src/thread_error.c
BIN_VALIDATION = test_validation
ASAN_BIN_VALIDATION = test_validation_asan
TSAN_BIN_VALIDATION = test_validation_tsan

.PHONY: all release asan tsan coverage clean

all: release asan tsan

release: $(BIN_VALIDATION)

asan: $(ASAN_BIN_VALIDATION)

tsan: $(TSAN_BIN_VALIDATION)

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
	rm -f $(BIN_VALIDATION) $(ASAN_BIN_VALIDATION) $(TSAN_BIN_VALIDATION) *.gcda *.gcno *.gcov ../src/*.gcda ../src/*.gcno
