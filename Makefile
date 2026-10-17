ZSTD_DIR = deps/zstd-1.5.7
ZSTD_TGZ = zstd-1.5.7.tar.gz
CC = cc
CFLAGS = -O2 -Wall
LDFLAGS = -lm -lpthread

all: build/tans_bench build/lzmini build/bzrouter

$(ZSTD_DIR)/lib/libzstd.a:
	@mkdir -p deps
	cd deps && [ -f $(ZSTD_TGZ) ] || curl -sL -o $(ZSTD_TGZ) https://github.com/facebook/zstd/releases/download/v1.5.7/$(ZSTD_TGZ)
	cd deps && [ -d $(ZSTD_DIR) ] || tar xzf $(ZSTD_TGZ)
	$(MAKE) -C $(ZSTD_DIR)/lib -j2 lib

build/tans_bench: src/tans_bench.c | build
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

build/lzmini: src/lzmini.c | build
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

build/bzrouter: src/bzrouter.c $(ZSTD_DIR)/lib/libzstd.a | build
	$(CC) $(CFLAGS) -o $@ $< -I$(ZSTD_DIR)/lib $(ZSTD_DIR)/lib/libzstd.a $(LDFLAGS)

build/zstd-cli: $(ZSTD_DIR)/lib/libzstd.a | build
	$(MAKE) -C $(ZSTD_DIR)/programs -j2 zstd
	@mkdir -p build && cp $(ZSTD_DIR)/programs/zstd build/zstd

build:
	mkdir -p build

clean:
	rm -rf build

.PHONY: all clean
