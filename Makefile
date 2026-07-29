CC = gcc
CFLAGS = -std=gnu2x -Wall -Wextra -Wpadded -Wdangling-pointer=2 -Wreturn-local-addr -Wpedantic -Wshadow -Werror -Wformat=2 -Wvla -Wimplicit-fallthrough -Wnull-dereference -Wundef -O3 -D_GNU_SOURCE -D_FORTIFY_SOURCE=3 -fstack-protector-strong -fstack-clash-protection -fcf-protection=full -march=x86-64-v3 -fPIE -Iinclude
LDFLAGS = -levent_pthreads -levent -ljson-c -lcurl -lodbc -lpthread -lsodium -lqrencode -loath -pie -Wl,-z,relro,-z,now

SRC = src/main.c src/server.c src/handlers.c src/http_client.c src/customer.c src/validation.c src/jwt.c src/odbcutil.c src/logger.c src/config.c src/worker_pool.c src/task_pool.c src/login.c src/totp.c src/json_util.c src/thread_error.c
OBJ = $(patsubst src/%.c,obj/%.o,$(SRC))
TARGET = bin/apiserver

.PHONY: all release clean asan tsan valgrind

all: release

release: $(TARGET)

$(TARGET): $(OBJ)
	@mkdir -p bin
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)



asan:
	@mkdir -p bin
	$(CC) -std=gnu2x -Wall -Wextra -Wpedantic -g -O1 -fsanitize=address,leak -D_GNU_SOURCE -Iinclude $(SRC) -o bin/test_asan $(LDFLAGS)

tsan:
	sudo sysctl vm.mmap_rnd_bits=30
	@mkdir -p bin
	$(CC) -std=gnu2x -Wall -Wextra -Wpedantic -g -O1 -fsanitize=thread -D_GNU_SOURCE -Iinclude $(SRC) -o bin/test_tsan $(LDFLAGS)

valgrind:
	@mkdir -p bin
	$(CC) -std=gnu2x -Wall -Wextra -Wpedantic -g -O1 -D_GNU_SOURCE -Iinclude $(SRC) -o bin/test_valgrind $(LDFLAGS)

obj/%.o: src/%.c
	@mkdir -p obj
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf obj
	rm -f bin/apiserver bin/test_asan bin/test_tsan bin/test_valgrind
