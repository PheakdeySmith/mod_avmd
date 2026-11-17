# AMVD top level
#   make            build engine CLI
#   make test       synthetic corpus + invariance + sanitizer tests
#   make module     build mod_amvd.so (see mod_amvd/Makefile for FS paths)
CC ?= gcc
CFLAGS ?= -O2 -g
CFLAGS += -std=c99 -D_POSIX_C_SOURCE=200809L -Wall -Wextra

all: build/amvd_cli

build/amvd_cli: tools/amvd_cli.c engine/amvd_engine.c engine/amvd_engine.h
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tools/amvd_cli.c engine/amvd_engine.c -lm

build/chunk_test: test/chunk_test.c engine/amvd_engine.c engine/amvd_engine.h
	@mkdir -p build
	$(CC) -std=c99 -g -fsanitize=address,undefined -o $@ test/chunk_test.c engine/amvd_engine.c -lm

test: build/amvd_cli build/chunk_test
	./build/chunk_test
	python3 test/gen_corpus.py build/corpus -n 30
	cd build/corpus && ../amvd_cli -f csv -s wait_message_end=1 *.wav > ../results.csv
	python3 tools/evaluate.py build/corpus/labels.csv build/results.csv

module:
	$(MAKE) -C mod_amvd

clean:
	rm -rf build
	$(MAKE) -C mod_amvd clean

.PHONY: all test module clean
