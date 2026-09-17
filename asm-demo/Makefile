# asm-demo — teaching playground: hand-written C demos and their generated
# assembly dumps.
#
# Only the *.c sources and this Makefile are tracked by git. The *.s dumps and
# the compiled Mach-O binaries are build artifacts and stay gitignored — you
# regenerate them locally with `make`.
#
# NOTE: the assembly is host/compiler specific. These dumps were produced with
# Apple clang on macOS/arm64 (`gcc` is clang there). On a different toolchain
# the output will differ, which is expected.

CC ?= gcc

# Default-opt dump for util.c, plus an -O2 variant, and -O2 dumps for the
# benchmark sources.
.PHONY: all dumps binaries clean

all: dumps binaries

dumps: util.s util_o2.s crlf_scan.s html_escape_bench.s memchr_bench.s

util.s: util.c
	$(CC) -S -o $@ $<

util_o2.s: util.c
	$(CC) -S -O2 -o $@ $<

crlf_scan.s: crlf_scan.c
	$(CC) -S -O2 -o $@ $<

html_escape_bench.s: html_escape_bench.c
	$(CC) -S -O2 -o $@ $<

memchr_bench.s: memchr_bench.c
	$(CC) -S -O2 -o $@ $<

binaries: crlf_scan html_escape_bench memchr_bench

crlf_scan: crlf_scan.c
	$(CC) -O2 -o $@ $<

html_escape_bench: html_escape_bench.c
	$(CC) -O2 -o $@ $<

memchr_bench: memchr_bench.c
	$(CC) -O2 -o $@ $<

clean:
	rm -f *.s crlf_scan html_escape_bench memchr_bench
