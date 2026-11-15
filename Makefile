# nwzh — NWN:EE Linux 中文渲染补丁
#
#   make          构建 libnwzh.so
#   make test     构建并运行解码器单元测试
#   make install  装到 $(PREFIX)（默认 ~/.local/lib/nwn-zh）
#   make clean    清理产物

CC       ?= cc
CFLAGS   ?= -O2 -Wall -Wextra
PREFIX   ?= $(HOME)/.local/lib/nwn-zh

LIB      := libnwzh.so
SRC      := src/nwzh.c
TOOLS    := tools/test_decode tools/tdec

# 目标进程是 ELF64 PIE，补丁要求位置无关，故用 -fPIC -shared
SOFLAGS  := -shared -fPIC
LDLIBS   := -ldl -lpthread

.PHONY: all test install clean

all: $(LIB)

$(LIB): $(SRC)
	$(CC) $(SOFLAGS) $(CFLAGS) -o $@ $< $(LDLIBS)
	@echo "built: $(CURDIR)/$(LIB)"

tools/%: tools/%.c
	$(CC) $(CFLAGS) -o $@ $< -ldl

test: $(LIB) tools/test_decode
	@./tests/run-decode-tests.sh

install: $(LIB)
	install -d $(PREFIX)
	install -m 0755 $(LIB) $(PREFIX)/$(LIB)
	@echo "installed: $(PREFIX)/$(LIB)"
	@echo "Steam 启动选项："
	@echo '  env NWNZH_TOUTF8=1 LD_PRELOAD=$(HOME)/.local/lib/nwn-zh/libnwzh.so %command%'

clean:
	rm -f $(LIB) $(TOOLS)
