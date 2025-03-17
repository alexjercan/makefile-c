# Makefile

This is a clone of Makefile, but really dumbed down.

It even uses itself :)

### Quickstart

```bash
make
```

### Example Syntax

```
build: main;
ds.o: ds.c ds.h = "clang ds.c -c -o ds.o";
main.o: main.c ds.h = "clang main.c -c -o main.o";
main: ds.o main.o = "clang ds.o main.o -o main";
```
