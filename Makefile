.DEFAULT_GOAL := build

.PHONY: help build test bench configure clean

help:
	./tools/build.sh help

build:
	./tools/build.sh build

test:
	./tools/build.sh test

bench:
	./tools/build.sh bench

configure:
	./tools/build.sh configure

clean:
	./tools/build.sh clean
