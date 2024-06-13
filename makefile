CFLAGS = -Wall -Wextra -pedantic -g -std=gnu99 -I/local/courses/csse2310/include
LINKED_LIB = -L/local/courses/csse2310/lib -lcsse2310a3

all: uqfindexec
uqfindexec: uqfindexec.c
	gcc $(CFLAGS) -o uqfindexec uqfindexec.c $(LINKED_LIB)

clean:
	rm uqfindexec
