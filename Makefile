CC=clang
CFLAGS=-Wall -Werror -Wextra -Wno-unused-function -g

build: main

ds.o: ds.c ds.h
	$(CC) $(CFLAGS) -c $< -o $@

main.o: main.c ds.h
	$(CC) $(CFLAGS) -c $< -o $@

main: ds.o main.o
	$(CC) $^ -o $@

clean:
	rm -f main main.o ds.o

.PHONY: build clean
