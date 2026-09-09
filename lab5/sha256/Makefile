# Makefile for sha256_example

CC = gcc
CFLAGS = -Wall -Wextra
LDFLAGS = -lssl -lcrypto

all: sha256_example

sha256_example: sha256_example.c
	$(CC) $(CFLAGS) -o sha256_example sha256_example.c $(LDFLAGS)

clean:
	rm -f sha256_example
