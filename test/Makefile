CC = gcc
CFLAGS = -std=gnu2x -Wall -Wextra -Wpedantic -O1 -g -I../include --coverage
LDFLAGS = -ljson-c --coverage

SRC_VALIDATION = validation-test.c ../src/validation.c
BIN_VALIDATION = test_validation
ASAN_BIN_VALIDATION = test_validation_asan
TSAN_BIN_VALIDATION = test_validation_tsan

SRC_CONCURRENCY = concurrency-test.c ../src/worker_pool.c ../src/task_pool.c ../src/config.c ../src/logger.c ../src/json_util.c ../src/validation.c
BIN_CONCURRENCY = test_concurrency
ASAN_BIN_CONCURRENCY = test_concurrency_asan
TSAN_BIN_CONCURRENCY = test_concurrency_tsan

.PHONY: all release asan tsan coverage clean

all: release asan tsan

release: $(BIN_VALIDATION) $(BIN_CONCURRENCY)

asan: $(ASAN_BIN_VALIDATION) $(ASAN_BIN_CONCURRENCY)

tsan: $(TSAN_BIN_VALIDATION) $(TSAN_BIN_CONCURRENCY)

coverage: $(BIN_VALIDATION)
	./$(BIN_VALIDATION)
	gcov-14 test_validation-validation.gcda

$(BIN_VALIDATION): $(SRC_VALIDATION)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(ASAN_BIN_VALIDATION): $(SRC_VALIDATION)
	$(CC) $(CFLAGS) -fsanitize=address,leak $^ -o $@ $(LDFLAGS)

$(TSAN_BIN_VALIDATION): $(SRC_VALIDATION)
	sudo sysctl vm.mmap_rnd_bits=30
	$(CC) $(CFLAGS) -fsanitize=thread $^ -o $@ $(LDFLAGS)

$(BIN_CONCURRENCY): $(SRC_CONCURRENCY)
	$(CC) $(CFLAGS) $^ -o $@ -levent_pthreads -levent -ljson-c -lcurl -lodbc -lpthread -lsodium -lqrencode

$(ASAN_BIN_CONCURRENCY): $(SRC_CONCURRENCY)
	$(CC) $(CFLAGS) -fsanitize=address,leak $^ -o $@ -levent_pthreads -levent -ljson-c -lcurl -lodbc -lpthread -lsodium -lqrencode

$(TSAN_BIN_CONCURRENCY): $(SRC_CONCURRENCY)
	sudo sysctl vm.mmap_rnd_bits=30
	$(CC) $(CFLAGS) -fsanitize=thread $^ -o $@ -levent_pthreads -levent -ljson-c -lcurl -lodbc -lpthread -lsodium -lqrencode

clean:
	rm -f $(BIN_VALIDATION) $(ASAN_BIN_VALIDATION) $(TSAN_BIN_VALIDATION) $(BIN_CONCURRENCY) $(ASAN_BIN_CONCURRENCY) $(TSAN_BIN_CONCURRENCY) *.gcda *.gcno *.gcov ../src/*.gcda ../src/*.gcno
