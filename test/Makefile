CC = gcc
CFLAGS = -std=gnu2x -Wall -Wextra -Wpedantic -O1 -g -I../include --coverage
LDFLAGS = -ljson-c --coverage

SRC = validation-test.c ../src/validation.c
BIN = test_validation
ASAN_BIN = test_validation_asan
TSAN_BIN = test_validation_tsan

.PHONY: all release asan tsan coverage clean

all: release asan tsan

release: $(BIN)

asan: $(ASAN_BIN)

tsan: $(TSAN_BIN)

coverage: $(BIN)
	./$(BIN)
	gcov-14 test_validation-validation.gcda

$(BIN): $(SRC)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(ASAN_BIN): $(SRC)
	$(CC) $(CFLAGS) -fsanitize=address,leak $^ -o $@ $(LDFLAGS)

$(TSAN_BIN): $(SRC)
	sudo sysctl vm.mmap_rnd_bits=30
	$(CC) $(CFLAGS) -fsanitize=thread $^ -o $@ $(LDFLAGS)

clean:
	rm -f $(BIN) $(ASAN_BIN) $(TSAN_BIN) *.gcda *.gcno *.gcov ../src/*.gcda ../src/*.gcno
