CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11
LDFLAGS ?= -lm
PREFIX  ?= $(HOME)/.local

stlview: stlview.c
	$(CC) $(CFLAGS) -o $@ stlview.c $(LDFLAGS)

# Generate test models (cube.stl, torus.stl) to develop against.
testmodels:
	python3 gen_test_stl.py

install: stlview
	mkdir -p $(PREFIX)/bin
	cp stlview $(PREFIX)/bin/stlview

clean:
	rm -f stlview cube.stl torus.stl

.PHONY: testmodels install clean
