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
BUILD_DIR = $(LIBZ_DIR)/build
TARGET    = ptgzip
TARGETS   = $(TARGET) pxgzip plgzip
SRC       = ptgzip.c

CC       ?= gcc
CFLAGS   ?= -O2 -s
THREADS  ?= $(shell nproc 2>/dev/null || echo 4)

.PHONY: all clean distclean source

all: $(TARGETS)
	@echo
	@for i in $(TARGETS); do du -k $$i; ldd $$i; echo; done

# -----------------------------------------------------------------------------
# source target: download tarball, extract, rename to libz/, build static lib
# -----------------------------------------------------------------------------
source: $(LIBZ_DIR)/libz.a

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

$(LIBZ_DIR)/libz.a: $(LIBZ_DIR)
	@which cmake >/dev/null 2>&1 || \
		{ echo "Error: cmake is required to build zlib-ng"; exit 1; }
	@echo ">>> Configuring zlib-ng (ZLIB_COMPAT=ON, tuned for density) ..."
	@cmake -S $(LIBZ_DIR) -B $(BUILD_DIR) \
		-DWITH_OPTIM=ON \
		-DZLIB_COMPAT=ON \
		-DBUILD_TESTING=OFF \
		-DBUILD_SHARED_LIBS=OFF \
		-DWITH_NEW_STRATEGIES=OFF \
		-DCMAKE_BUILD_TYPE=Release
	@echo ">>> Building zlib-ng with $(THREADS) jobs ..."
	@cmake --build $(BUILD_DIR) --parallel $(THREADS)
	@cp $(BUILD_DIR)/libz*.a $@
	@echo ">>> Built: $@"

# -----------------------------------------------------------------------------
# ptgzip: compile against local zlib-ng headers and static archive
# -----------------------------------------------------------------------------
$(TARGET): $(SRC) $(LIBZ_DIR)/libz.a
	$(CC) $(CFLAGS) -o $@ $< \
		-I$(BUILD_DIR) -I$(LIBZ_DIR) \
		$(LIBZ_DIR)/libz.a -lpthread

pxgzip: pxgzip.c
	$(CC) $(CFLAGS) -o $@ $<

plgzip: ptgzip.c
	$(CC) $(CFLAGS) -o $@ $< -lz -lpthread

# -----------------------------------------------------------------------------
# Cleanup
# -----------------------------------------------------------------------------
clean:
	rm -f $(TARGETS)

veryclean: clean
	rm -rf libz

distclean: veryclean
	rm -f $(TARBALL)

