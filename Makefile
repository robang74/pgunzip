# ptgzip Makefile with embedded zlib-ng build
#
#   make source    # download, extract, rename and build zlib-ng
#   make           # build ptgzip (triggers source automatically if needed)
#   make clean     # remove ptgzip binary only
#   make distclean # remove binary, libz/ source tree and tarball

VERSION  ?= 2.3.3
ZARCHIVE  = $(VERSION).tar.gz
TARBALL   = zlib-ng-$(VERSION).tar.gz
REPOURL   = https://github.com/robang74
ZURL      = $(REPOURL)/zlib-ng/archive/refs/tags/$(ZARCHIVE)
BARCHIVE  = uchaosys.zip
BURL      = $(REPOURL)/busybox/archive/refs/heads/$(BARCHIVE)
LIBZ_DIR  = libz
LIBZ_A    = $(LIBZ_DIR)/libz.a
MINZ_DIR  = minz/amalgamation
BUILD_DIR = $(LIBZ_DIR)/build
TARGET    = ptgzip
TARGETS   = pxgzip plgzip bbox/gzip pmgzip pugzip $(TARGET)
GZCMD     = $(shell command -v pigz gzip | head -n1)
SRC       = ptgzip.c
NTS      ?= 30

CC       ?= gcc
CFLAGS   ?= -g0 -O2 -s -falign-functions=32 -flto -mavx2 $(EXTRA_CFLAGS)
THREADS  ?= $(shell nproc 2>/dev/null || echo 4)

MINZ_ARGS = -Wl,--defsym=deflateInit2_=mz_deflateInit2
MINZ_ARGS+= -Wl,--defsym=deflateBound=mz_deflateBound
MINZ_ARGS+= -Wl,--defsym=deflateEnd=mz_deflateEnd
MINZ_ARGS+= -Wl,--defsym=deflate=mz_deflate

.PHONY: all clean distclean source

all: $(TARGETS)
	@echo
	@for i in $(TARGETS); do du -k $$i; ldd $$i; echo; done
	@du -k libzall.a && printf "\t%s\n\n" "$$(file libzall.a)"

# -----------------------------------------------------------------------------
# source target: download tarball, extract, rename to libz/, build static lib
# -----------------------------------------------------------------------------
source: $(LIBZ_A)

$(TARBALL):
	@echo ">>> Downloading $@ ..."
	wget -O $@ $(ZURL) || curl -Lo $@ $(ZURL) || \
		{ echo "Error: install wget or curl"; rm -f $@; exit 1; }

busybox.zip:
	@echo ">>> Downloading $@ ..."
	wget -O $@ $(BURL) || curl -Lo $@ $(BURL) || \
		{ echo "Error: install wget or curl"; rm -f $@; exit 1; }

$(LIBZ_DIR): $(TARBALL)
	@echo ">>> Extracting $(TARBALL) -> $(LIBZ_DIR)/"
	rm -rf zlib-ng-2.3.3 libz/
	tar xzf $(TARBALL)
	mv zlib-ng-2.3.3 $(LIBZ_DIR)
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

# cmake -S . -B build -D MZ_BUILD_TESTS=OFF -D MZ_LIBCOMP=OFF -D MZ_FETCH_LIBS=OFF -D MZ_PKCRYPT=OFF -D MZ_WZAES=OFF -D MZ_OPENSSL=OFF -D MZ_LIBBSD=OFF -D MZ_ICONV=OFF -D MZ_BZIP2=OFF -D MZ_LZMA=OFF -D MZ_PPMD=OFF -D MZ_ZSTD=OFF
# cmake --build build

bbox: busybox.zip
	unzip -q $^ && mv -f busybox-uchaosys/ $@/
	cp -f $@/ubuntu/config.gzip $@/.config

bbox/gzip: bbox/.config | bbox
	cd bbox && make -j && mv busybox gzip

# -----------------------------------------------------------------------------
# ptgzip: compile against native zlib-ng headers and static archive
# -----------------------------------------------------------------------------
$(TARGET): $(SRC) libzall.a
	$(CC) -o $@ $< -I$(BUILD_DIR) -I$(LIBZ_DIR) libzall.a \
	  -D_USE_ZNG=0 -lpthread $(CFLAGS)

pxgzip: pxgzip.c
	$(CC) $(CFLAGS) -o $@ $<

plgzip: ptgzip.c
	$(CC) $(CFLAGS) -o $@ $< -lz -lpthread -D_USE_ZLIB

minz/.sync:
	git submodule update --init --recursive
	touch $@

updateminz: | minz/.sync
	@echo "Updating miniz at the rfc1952 branch HEAD"
	cd minz && git fetch origin rfc1952 \
	  && git checkout --force FETCH_HEAD
	cd minz/ && cat 00*.patch | patch -p1 \
	  && git status | grep modified
	rm -rf libzall.a minz/amalgamation/

ungz/.sync:
	git submodule update --init --recursive
	touch $@

$(MINZ_DIR)/miniz.c: | minz/.sync
	cd minz && SKIPTESTS=1 sh amalgamate.sh

$(MINZ_DIR)/miniz.c.o: $(MINZ_DIR)/miniz.c
	$(CC) $(CFLAGS) -c $< -o $<.o

minz/libminz.a: $(MINZ_DIR)/miniz.c.o
	$(AR) rcs $@ $<

ungz/libungz.a: | ungz/.sync
	make -C ungz libungz.a

libmerge.mri: $(LIBZ_A) minz/libminz.a ungz/libungz.a
	@echo "CREATE libzall.a" > $@
	@for i in $^; do echo "ADDLIB $$i"; done >> $@
	@printf "SAVE\nEND\n" >> $@

libzall.a: libmerge.mri
	@$(AR) -M < $^

pmgzip: ptgzip.c libzall.a
	$(CC) $(CFLAGS) -o $@ $^ -I$(MINZ_DIR) -lpthread -D_USE_MNZ=1

pugzip: ptgzip.c libzall.a
	$(CC) $(CFLAGS) -o $@ $^ -lpthread -D_USE_UNGZ=1

# -----------------------------------------------------------------------------
# Tests
# -----------------------------------------------------------------------------

.PHONY: tests blkline speed stress iocat crash devel
.PHONY:  test-clean  test-basic  test-speed  test-pigzc
.PHONY: _test-clean _test-basic _test-speed _test-pigzc
.PHONY:  test-speef  test-gzipc  test-crash  test-pigzf
.PHONY: _test-speef _test-gzipc _test-crash _test-pigzf
.PHONY:  test-gzipf _test-gzipf  test-zsize _test-zsize
.PHONY: _test-inout  test-inout _speed-stress speed-stress

CRASH_FLAGS ?= -D_THR_WAIT=1 -D_GZ_WRITE=0 -D_USE_MMAP=1 -D_USE_FREE=0
IOWAY_FLAGS ?= -D_THR_WAIT=0 -D_GZ_WRITE=1 -D_USE_MMAP=0 -D_USE_FREE=1

NPROC ?= 4
CMD2T ?= ./ptgzip
CMDVF  =
CMDVC  = -c
ifeq ($(CMD2T),./pxgzip)
CMDVF  = > libz.tar.gz
CMDVC  =
endif

libz.tar: /bin/tar
	@/bin/tar cf libz.tar libz

libz.tar.gz: libz.tar | ptgzip
#	$(GZCMD) -nk libz.tar
	./ptgzip -nk libz.tar

blkline:
	@echo

tests: test-basic _test-speed _test-pigzc blkline _test-speef _test-gzipf
	@echo

devel: $(LIBZ_A)
	@printf "\n=========================="
	@printf "\n=== test clean + basic ==="
	@printf "\n==========================\n\n"
	@make _test-clean || printf "\n>>> ERR=$$?\n"
	@make  test-basic || printf "\n>>> ERR=$$?\n"
	@printf "\n=========================="
	@printf "\n=== test clean + gunzp ==="
	@printf "\n==========================\n\n"
	@make  test-gunzp || printf "\n>>> ERR=$$?\n"
	@printf "\n=========================="
	@printf "\n=== test clean + iocat ==="
	@printf "\n==========================\n\n"
	@make iocat       || printf "\n>>> ERR=$$?\n"
	@make test-basic  || printf "\n>>> ERR=$$?\n"
	@printf "\n=========================="
	@printf "\n=== test clean + crash ==="
	@printf "\n==========================\n\n"
	@make crash       || printf "\n>>> ERR=$$?\n"
	@make test-basic  || printf "\n>>> ERR=$$?\n"
	@echo

speed:
	@make _test-speed _test-speef CMD2T=$(CMD2T) blkline
	@echo

speed-stress: libz.tar $(CMD2T)
	@printf "\n=== $(CMD2T) '-c' speed test on /bin/ ===\n\n"
	@cmd='for i in $$list; do $(CMD2T) $$i $(CMDVC); done' \
    && sync && echo "$$cmd" && nl=/dev/null \
    && list="$(shell find /bin/ -type f | sort)" \
    && time eval "$$cmd" | dd bs=1M of=$$nl
	@echo

stress-speed: speed-stress

stress: libz.tar $(CMD2T)
	@printf "\n=== $(CMD2T) '-c' stress test on /bin/ ===\n\n"
	@cmd='for i in $$list; do $(CMD2T) $$i $(CMDVC) -1 | zcat; done' \
    && sync && echo "$$cmd" && nl=/dev/null \
    && list="$(shell find /bin/ -type f | sort)" \
    && time eval "$$cmd" >$$nl
	@echo

_speed-gunzp: libz.tar.gz $(CMD2T)
	@printf "\n=== $(CMD2T) '-dc' speed test x$(NTS) ===\n\n"
	nl=/dev/null && cmd="$(CMD2T) libz.tar.gz -dkf $(CMDVC)" && sync && \
    eval "$$cmd ||:" >$$nl && time for i in $$(seq 1 $(NTS)); do \
    eval "$$cmd 2>&-"; done | dd bs=1M of=$$nl

speed-gunzp: _speed-gunzp blkline

_speef-gunzp: libz.tar.gz $(CMD2T)
	@printf "\n=== $(CMD2T) '-dk' speed test x$(NTS) ===\n\n"
	nl=/dev/null && cmd="$(CMD2T) libz.tar.gz -dkf $(NP)" && sync && \
    eval "$$cmd ||:" >$$nl && time for i in $$(seq 1 $(NTS)); do \
    eval "$$cmd"; done; sync

speef-gunzp: _speef-gunzp blkline

crash:
	@printf "\n=== iocat build w/ CRASH_FLAGS ===\n\n"
	rm -f ptgzip && make ptgzip EXTRA_CFLAGS="$(EXTRA_CFLAGS) $(CRASH_FLAGS)"

iocat:
	@printf "\n=== iocat build w/ IOWAY_FLAGS ===\n\n"
	rm -f ptgzip && make ptgzip EXTRA_CFLAGS="$(EXTRA_CFLAGS) $(IOWAY_FLAGS)"

_stress-iocat:
	@printf "\n=== iocat stress test on /bin/ ===\n\n"
	@cmd='for i in $$list; do cat $$i | $(CMD2T) $(CMDVC) -1 | zcat; done' \
    && sync && echo "$$cmd" && nl=/dev/null \
    && list="$(shell find /bin/ -type f | sort)" \
    && time eval "$$cmd" >$$nl
	@echo

stress-iocat: iocat _stress-iocat blkline

iocat-stress: stress-iocat

/bin/tar:
	@printf "\nERROR: $@ not installed, abort\n\n"
	false

/bin/pigz:
	@printf "\nERROR: $@ not installed, abort\n\n"
	false

/bin/gzip:
	@printf "\nERROR: $@ not installed, abort\n\n"
	false

_test-clean:
	@printf "\n=== $(CMD2T) compilation test ===\n\n"
	@rm -f libz.tar libz.tar.gz
	@rm -f $$(basename $(CMD2T)) ||:
	@echo make $(CMD2T) | grep -q "/bin/" ||\
    make $$(basename $(CMD2T))

test-clean: _test-clean blkline

WZCAT := { ./ptgzip -kdc | tee test.dz | wc -c; }

_test-basic: libz.tar $(CMD2T) ptgzip
	@printf "\n=== $(CMD2T) '-c' sanity check ===\n\n"
	$(CMD2T) libz.tar -kfv $(CMDVC) | $(WZCAT)
	@diff test.dz libz.tar && echo ">>> Result: OK"
	@rm -f test.dz
	@printf "\n=== $(CMD2T) stdin sanity check ===\n\n"
	cat libz.tar | $(CMD2T) -v $(CMDVC) | $(WZCAT)
	@diff test.dz libz.tar && echo ">>> Result: OK"
	@rm -f test.dz
	@printf "\n=== $(CMD2T) file sanity check ===\n\n"
	$(CMD2T) libz.tar -kfv $(CMDVF) && du -b libz.tar*
	cat libz.tar.gz | $(WZCAT)
	@diff test.dz libz.tar && echo ">>> Result: OK"
	@rm -f test.dz

test-basic: _test-basic blkline

_test-ptgz:
	@printf "\n=== ./ptgzip PTGZ sanity check ===\n\n"
	rm -f libz.tar libz.tar.gz
	make -j libz.tar ptgzip >/dev/null
	@echo
	./ptgzip libz.tar -kv
	head -c64 libz.tar.gz | hexdump -C
	@echo
	./ptgzip -dt libz.tar.gz
	zcat libz.tar.gz | wc -c; echo ret=$$?

test-ptgz: _test-ptgz blkline

_test-gunzp: libz.tar.gz $(CMD2T)
	@printf "\n=== $(CMD2T) gunzp sanity check ===\n\n"
	cat libz.tar.gz | $(CMD2T) -dkfv $(CMDVC) | tee test.dz | wc -c
	@diff test.dz libz.tar && echo ">>> Result: OK"
	@rm -f test.dz

test-gunzp: _test-gunzp blkline

_test-inout: libz.tar libz.tar.gz $(CMD2T)
	@printf "\n=== $(CMD2T) I/O speed test x$(NTS) ===\n\n"
	nl=/dev/null && cmd="dd if=libz.tar bs=1M status=none |\
	    $(CMD2T) $(CMDVC)" && sync && \
    eval "$$cmd" >$$nl && time for i in $$(seq 1 $(NTS)); do \
    eval "$$cmd"; done | dd bs=1M of=$$nl
	@printf "\n=== $(CMD2T) -d I/O speed test x$(NTS) ===\n\n"
	nl=/dev/null && cmd="dd if=libz.tar.gz bs=1M status=none |\
	    $(CMD2T) -d $(CMDVC)" && sync && \
    eval "$$cmd" >$$nl && time for i in $$(seq 1 $(NTS)); do \
    eval "$$cmd"; done | dd bs=1M of=$$nl

test-inout: _test-inout blkline

_test-speed: libz.tar $(CMD2T)
	@printf "\n=== $(CMD2T) '-c' speed test x$(NTS) ===\n\n"
	nl=/dev/null && cmd="$(CMD2T) libz.tar -kf $(CMDVC)" && sync && \
    eval "$$cmd" >$$nl && time for i in $$(seq 1 $(NTS)); do \
    eval "$$cmd"; done | dd bs=1M of=$$nl

test-speed: _test-speed blkline

_test-speef: libz.tar $(CMD2T)
	@printf "\n=== $(CMD2T) file speed test x$(NTS) ===\n\n"
	nl=/dev/null && cmd="$(CMD2T) libz.tar -kf $(CMDVF) $(NP)" && sync && \
    eval "$$cmd" && time for i in $$(seq 1 $(NTS)); do \
    eval "$$cmd"; done; sync

test-speef: _test-speef blkline

_test-gzipc: libz.tar /bin/gzip
	@printf "\n=== gzip '-c' compare test x$(NTS) ===\n\n"
	nl=/dev/null && cmd="/bin/gzip -knf -c libz.tar" && sync && \
    eval "$$cmd" >$$nl && time for i in $$(seq 1 $(NTS)); do \
    eval "$$cmd"; done | dd bs=1M of=$$nl

test-gzipc: _test-gzipc blkline

_test-gzipf: libz.tar /bin/gzip
	@printf "\n=== gzip file compare test x$(NTS) ===\n\n"
	nl=/dev/null && cmd="/bin/gzip -knf libz.tar" && sync && \
    eval "$$cmd" >$$nl && time for i in $$(seq 1 $(NTS)); do \
    eval "$$cmd"; done
	@sync

test-gzipf: _test-gzipf blkline

_test-pigzf: libz.tar /bin/pigz
	@printf "\n=== pigz file compare test x$(NTS) ===\n\n"
	nl=/dev/null && cmd="/bin/pigz -knmf libz.tar" && sync && \
    eval "$$cmd" && time for i in $$(seq 1 $(NTS)); do \
    eval "$$cmd"; done
	@sync

test-pigzf: _test-pigzf blkline

_test-pigzc: libz.tar /bin/pigz
	@printf "\n=== pigz '-c' compare test x$(NTS) ===\n\n"
	nl=/dev/null && cmd="/bin/pigz -knmf -c libz.tar" && sync && \
    eval "$$cmd" >$$nl && time for i in $$(seq 1 $(NTS)); do \
    eval "$$cmd"; done | dd bs=1M of=$$nl

test-pigzc: _test-pigzc blkline

_test-crash: crash
	@printf "\n=== alternative test w/ CRASH_FLAGS ===\n\n"
	make _test-basic _test-speed _test-speef blkline # NP=-p$(NPROC)
# make _test-basic _test-speed _test-pigzc _test-speef _test-pigzf blkline
	@sync

test-crash: _test-crash blkline

_test-zsize: libz.tar ptgzip
	@printf "\n=== compress size/time comparison (zlib-ng) ===\n"
	@printf "\n>>> NOTE: for a fair comparison 'pigz -p6', isn't requried anymore.\n\n"
	@rm -f libz.tar.gz pigz-?.gz gzip-?.gz ptgz-?.gz
	{ time ./ptgzip      -9k    -c libz.tar > ptgz-9.gz; } 2>&1 | grep real
	{ time /bin/pigz     -9knmf -c libz.tar > pigz-9.gz; } 2>&1 | grep real
	{ time /bin/gzip     -9kn   -c libz.tar > gzip-9.gz; } 2>&1 | grep real
	@echo
	{ time ./ptgzip      -6k    -c libz.tar > ptgz-6.gz; } 2>&1 | grep real
	{ time /bin/pigz     -6knmf -c libz.tar > pigz-6.gz; } 2>&1 | grep real
	{ time /bin/gzip     -6kn   -c libz.tar > gzip-6.gz; } 2>&1 | grep real
	@echo
	{ time ./ptgzip      -3k    -c libz.tar > ptgz-3.gz; } 2>&1 | grep real
	{ time /bin/pigz     -3knmf -c libz.tar > pigz-3.gz; } 2>&1 | grep real
	{ time /bin/gzip     -3kn   -c libz.tar > gzip-3.gz; } 2>&1 | grep real
	@echo
	for i in 9 6 3; do { echo; du -b *[zp]-$$i.gz; }| sort -n; done # groups

test-zsize: _test-zsize blkline

# -----------------------------------------------------------------------------
# Cleanup
# -----------------------------------------------------------------------------
clean:
	rm -f $(TARGETS) libz.tar libz.tar.gz test.dz

veryclean: clean
	rm -rf libz minz/.sync minz/amalgamation libzall.a

distclean: veryclean
	rm -f $(TARBALL)

