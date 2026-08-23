CC = gcc
AR = gcc-ar
CFLAGS = -std=gnu2x -Wall -Wextra -Wpadded -Wdangling-pointer=2 -Wreturn-local-addr -Wpedantic -Wshadow -Werror -Wformat=2 -Wvla -Wimplicit-fallthrough -Wnull-dereference -Wundef -g -flto=auto -O3 -D_GNU_SOURCE -D_FORTIFY_SOURCE=3 -fstack-protector-strong -fstack-clash-protection -fcf-protection=full -march=x86-64-v3 -fPIE -Iinclude
LDFLAGS = -levent_pthreads -levent -ljson-c -lcurl -lodbc -lpthread -lsodium -lqrencode -loath -pie -Wl,-z,relro,-z,now

LIB_SRC = $(wildcard lib/*.c)
LIB_OBJ = $(patsubst lib/%.c,obj/%.o,$(LIB_SRC))
LIB_TARGET = bin/libapiserver.a

APP_SRC = $(wildcard src/*.c)
APP_OBJ = $(patsubst src/%.c,obj/%.o,$(APP_SRC))
APP_TARGET = bin/apiserver

.PHONY: all release slim clean asan tsan valgrind lib

all: release

release: $(APP_TARGET)

lib: $(LIB_TARGET)

slim: CFLAGS := $(filter-out -g,$(CFLAGS))
slim: $(APP_TARGET)
	strip -s $(APP_TARGET)

$(LIB_TARGET): $(LIB_OBJ)
	@mkdir -p bin
	$(AR) rcs $@ $^

$(APP_TARGET): $(APP_OBJ) $(LIB_TARGET)
	@mkdir -p bin
	$(CC) $(CFLAGS) $(APP_OBJ) -Lbin -lapiserver -o $@ $(LDFLAGS)

asan:
	@mkdir -p bin
	$(CC) -std=gnu2x -Wall -Wextra -Wpedantic -g -O1 -fsanitize=address,leak -D_GNU_SOURCE -Iinclude $(APP_SRC) $(LIB_SRC) -o bin/test_asan $(LDFLAGS)

tsan:
	sudo sysctl vm.mmap_rnd_bits=30
	@mkdir -p bin
	$(CC) -std=gnu2x -Wall -Wextra -Wpedantic -g -O1 -fsanitize=thread -D_GNU_SOURCE -Iinclude $(APP_SRC) $(LIB_SRC) -o bin/test_tsan $(LDFLAGS)

valgrind:
	@mkdir -p bin
	$(CC) -std=gnu2x -Wall -Wextra -Wpedantic -g -O1 -D_GNU_SOURCE -Iinclude $(APP_SRC) $(LIB_SRC) -o bin/test_valgrind $(LDFLAGS)

obj/%.o: lib/%.c
	@mkdir -p obj
	$(CC) $(CFLAGS) -c $< -o $@

obj/%.o: src/%.c
	@mkdir -p obj
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf obj
	rm -f bin/apiserver bin/libapiserver.a bin/test_asan bin/test_tsan bin/test_valgrind
