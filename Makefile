# ptgzip Makefile with embedded zlib-ng build
#
#   make source    # download, extract, rename and build zlib-ng
#   make           # build ptgzip (triggers source automatically if needed)
#   make clean     # remove ptgzip binary only
#   make distclean # remove binary, libz/ source tree and tarball

VERSION  ?= 2.3.3
ARCHIVE   = $(VERSION).tar.gz
TARBALL   = zlib-ng-$(VERSION).tar.gz
REPOURL   = https://github.com/robang74/zlib-ng/archive/refs/tags
URL       = $(REPOURL)/$(ARCHIVE)
LIBZ_DIR  = libz
LIBZ_A    = $(LIBZ_DIR)/libz.a
MINZ_DIR  = minz/amalgamation
BUILD_DIR = $(LIBZ_DIR)/build
TARGET    = ptgzip
TARGETS   = $(TARGET) pxgzip plgzip pmgzip
SRC       = ptgzip.c

CC       ?= gcc
CFLAGS   ?= -g0 -O2 -s -falign-functions=32 $(EXTRA_CFLAGS)
THREADS  ?= $(shell nproc 2>/dev/null || echo 4)

MINZ_ARGS = -Wl,--defsym=deflateInit2_=mz_deflateInit2
MINZ_ARGS+= -Wl,--defsym=deflateBound=mz_deflateBound
MINZ_ARGS+= -Wl,--defsym=deflateEnd=mz_deflateEnd
MINZ_ARGS+= -Wl,--defsym=deflate=mz_deflate

.PHONY: all clean distclean source

all: $(TARGETS)
	@echo
	@for i in $(TARGETS); do du -k $$i; ldd $$i; echo; done

# -----------------------------------------------------------------------------
# source target: download tarball, extract, rename to libz/, build static lib
# -----------------------------------------------------------------------------
source: $(LIBZ_A)

$(TARBALL):
	@echo ">>> Downloading $(TARBALL) ..."
	@wget -q -O $@ $(URL) || curl -sL -o $@ $(URL) || \
		{ echo "Error: install wget or curl"; exit 1; }

$(LIBZ_DIR): $(TARBALL)
	@echo ">>> Extracting $(TARBALL) -> $(LIBZ_DIR)/"
	@rm -rf zlib-ng-2.3.3
	@tar xzf $(TARBALL)
	@mv zlib-ng-2.3.3 $(LIBZ_DIR)
	@touch $@

$(LIBZ_A): $(LIBZ_DIR)
	@which cmake >/dev/null 2>&1 || \
		{ echo "Error: cmake is required to build zlib-ng"; exit 1; }
	@echo ">>> Configuring zlib-ng (native API, ratio-tuned) ..."
	@cmake -S $(LIBZ_DIR) -B $(BUILD_DIR) \
		-DWITH_OPTIM=ON \
		-DZLIB_COMPAT=ON \
		-DWITH_GTEST=OFF \
		-DZLIB_ALIASES=OFF \
		-DWITH_GZFILEOP=OFF \
		-DBUILD_TESTING=OFF \
		-DBUILD_SHARED_LIBS=OFF \
		-DWITH_ALL_FALLBACKS=OFF \
		-DWITH_NEW_STRATEGIES=OFF \
		-DWITH_RUNTIME_CPU_DETECTION=ON \
		-DCMAKE_BUILD_TYPE=Release
	@echo ">>> Building zlib-ng with $(THREADS) jobs ..."
	@cmake --build $(BUILD_DIR) --parallel $(THREADS)
	@cp $(BUILD_DIR)/libz*.a $@
	@echo ">>> Built: $@"

# -----------------------------------------------------------------------------
# ptgzip: compile against native zlib-ng headers and static archive
# -----------------------------------------------------------------------------
$(TARGET): $(SRC) $(LIBZ_A)
	$(CC) -o $@ $< -I$(BUILD_DIR) -I$(LIBZ_DIR) $(LIBZ_A) \
	  -D_USE_ZNG=0 -lpthread $(CFLAGS)

pxgzip: pxgzip.c
	$(CC) $(CFLAGS) -o $@ $<

plgzip: ptgzip.c
	$(CC) $(CFLAGS) -o $@ $< -lz -lpthread -D_USE_ZLIB

minz/.sync:
	git submodule update --init --recursive
	touch minz/.sync

$(MINZ_DIR)/miniz.c: minz/.sync
	cd minz && sh amalgamate.sh

pmgzip: ptgzip.c $(MINZ_DIR)/miniz.c
	$(CC) $(CFLAGS) -o $@ $^ -I$(MINZ_DIR) -lpthread -D_USE_MNZ=1

# -----------------------------------------------------------------------------
# Tests
# -----------------------------------------------------------------------------

.PHONY: tests blkline
.PHONY:  test-clean  test-basic  test-speed  test-pigzc
.PHONY: _test-clean _test-basic _test-speed _test-pigzc
.PHONY:  test-speef  test-gzipc  test-crash  test-pigzf
.PHONY: _test-speef _test-gzipc _test-crash _test-pigzf
.PHONY:  test-gzipf _test-gzipf

blkline:
	@echo

tests: _test-clean _test-basic blkline _test-speed _test-gzip
	@echo

_test-clean:
	@printf "\n=== ptgzip compilation test ===\n\n"
	rm -f ptgzip && make ptgzip
	@rm -f libz.tar libz.tar.gz

test-clean: _test-clean blkline

_test-basic:
	@printf "\n=== ptgzip file sanity check ===\n\n"
	test -r libz.tar || tar cf libz.tar libz
	./ptgzip libz.tar && du -b libz.tar*
	zcat libz.tar.gz | tee test.dz | wc -c
	diff test.dz libz.tar && echo ">>> Result: OK"
	@rm -f test.dz
	@printf "\n=== ptgzip '-c' sanity check ===\n\n"
	./ptgzip -c libz.tar | zcat | tee test.dz | wc -c
	diff test.dz libz.tar && echo ">>> Result: OK"
	@rm -f test.dz

test-basic: _test-basic blkline

_test-speed:
	@printf "\n=== speed test preparation ===\n\n"
	test -r libz.tar || tar cf libz.tar libz
	@printf "\n=== ptgzip '-c' speed test ===\n\n"
	nl=/dev/null && cmd="./ptgzip -c libz.tar" && sync && \
    eval "$$cmd" >$$nl && time for i in $$(seq 1 30); do \
    eval "$$cmd"; done | dd bs=1M of=$$nl

test-speed: _test-speed blkline

_test-speef:
	@printf "\n=== speed test preparation ===\n\n"
	test -r libz.tar || tar cf libz.tar libz
	@printf "\n=== ptgzip file speed test ===\n\n"
	nl=/dev/null && cmd="./ptgzip libz.tar" && sync && \
    eval "$$cmd" && time for i in $$(seq 1 30); do \
    eval "$$cmd"; done

test-speef: _test-speef blkline

_test-gzipc:
	@printf "\n=== gzip '-c' compare test ===\n\n"
	test -r libz.tar || tar cf libz.tar libz
	nl=/dev/null && cmd="/bin/gzip -knc libz.tar" && sync && \
    eval "$$cmd" >$$nl && time for i in $$(seq 1 30); do \
    eval "$$cmd"; done | dd bs=1M of=$$nl

test-gzipc: _test-gzipc blkline

_test-gzipf:
	@printf "\n=== gzip '-c' compare test ===\n\n"
	test -r libz.tar || tar cf libz.tar libz
	nl=/dev/null && cmd="/bin/gzip -kn libz.tar" && sync && \
    eval "$$cmd" >$$nl && time for i in $$(seq 1 30); do \
    eval "$$cmd"; done | dd bs=1M of=$$nl

test-gzipf: _test-gzipf blkline

_test-pigzf:
	@printf "\n=== pigz file compare test ===\n\n"
	test -r libz.tar || tar cf libz.tar libz
	nl=/dev/null && cmd="/bin/pigz -p6 -knmf libz.tar" && sync && \
    eval "$$cmd" && time for i in $$(seq 1 30); do \
    eval "$$cmd"; done

test-pigzf: _test-pigzf blkline

_test-pigzc:
	@printf "\n=== pigz '-c' compare test ===\n\n"
	test -r libz.tar || tar cf libz.tar libz
	nl=/dev/null && cmd="/bin/pigz -p6 -knmf -c libz.tar" && sync && \
    eval "$$cmd" >$$nl && time for i in $$(seq 1 30); do \
    eval "$$cmd"; done | dd bs=1M of=$$nl

test-pigzc: _test-pigzc blkline

_test-crash:
	@printf "\n=== crash test _THR_WAIT=0 ===\n\n"
	rm -f ptgzip && make ptgzip EXTRA_CFLAGS="-D_THR_WAIT=0"
	make _test-basic _test-speed _test-pigzc _test-speef _test-pigzf blkline

test-crash: _test-crash blkline

# -----------------------------------------------------------------------------
# Cleanup
# -----------------------------------------------------------------------------
clean:
	rm -f $(TARGETS)

veryclean: clean
	rm -rf libz minz/.sync

distclean: veryclean
	rm -f $(TARBALL)

