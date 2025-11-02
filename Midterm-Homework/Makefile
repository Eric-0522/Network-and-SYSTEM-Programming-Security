# Makefile — build shared lib and two binaries

# 功能說明：
# 1. 這個 Makefile 可以自動建立整個 client-server 專案。
# 2. 它會編譯共用函式庫 (libutils.so)，以及兩個執行檔 (server 與 client)。
# 3. 支援兩層除錯控制：編譯期可用 ENABLE_DEBUG=1 啟用 DEBUG 宏，執行期則透過 LOG_LEVEL 環境變數或 -v 參數控制。
# 4. 自動建立必要的資料夾 (lib/, bin/)。
# 5. 支援 clean 指令清除編譯產物。
# 6. 所有目標皆依賴 include/ 與 src/ 下的程式碼。
# 7. LIBS 與 LDFLAGS 自動設定為載入共用函式庫 (rpath 設定確保執行時能找到 .so)。

CC      := gcc
CSTD    := -std=c11
OPT     := -O2
WARN    := -Wall -Wextra -Wpedantic -Wformat=2 -Wshadow -Wcast-qual -Wpointer-arith -Wwrite-strings -Wmissing-prototypes -Wstrict-prototypes -Wvla
PIC     := -fPIC
PREFIX  := $(CURDIR)
INCDIR  := $(PREFIX)/include
SRCDIR  := $(PREFIX)/src
LIBDIR  := $(PREFIX)/lib
BINDIR  := $(PREFIX)/bin

# 編譯期除錯開關：ENABLE_DEBUG=1 時會加入 -DENABLE_DEBUG 宏
ifeq ($(ENABLE_DEBUG),1)
CDEBUG := -DENABLE_DEBUG
else
CDEBUG :=
endif

# CFLAGS: 編譯選項 + include 路徑
CFLAGS  := $(CSTD) $(OPT) $(WARN) $(CDEBUG) -fno-common -D_GNU_SOURCE -I$(INCDIR)

# LDFLAGS: 指定執行時搜尋 lib 的路徑
LDFLAGS := -Wl,-rpath,$(LIBDIR) -L$(LIBDIR)

# LIBS: 要連結的共用函式庫名稱 (-lutils => libutils.so)
LIBS    := -lutils
UTIL_OBJS := $(SRCDIR)/common.o

# ===== 主目標 =====
# 建立資料夾、共用函式庫、server、client
all: dirs $(LIBDIR)/libutils.so $(BINDIR)/server $(BINDIR)/client

# ===== 幫助指令 =====
.PHONY: dirs clean

dirs:
	@mkdir -p $(LIBDIR) $(BINDIR)

# ===== 編譯共用函式庫 =====
# 先將 common.c 編譯成位置獨立物件，再組成 libutils.so
$(SRCDIR)/common.o: $(SRCDIR)/common.c $(INCDIR)/common.h
	$(CC) $(CFLAGS) $(PIC) -c -o $@ $<

$(LIBDIR)/libutils.so: $(UTIL_OBJS)
	$(CC) -shared -Wl,-soname,libutils.so -o $@ $^

# ===== 編譯 server =====
# 連結 libutils.so 並設定 rpath，讓執行時能找到該 so
$(BINDIR)/server: $(SRCDIR)/server.c $(INCDIR)/common.h $(LIBDIR)/libutils.so
	$(CC) $(CFLAGS) -o $@ $(SRCDIR)/server.c $(LDFLAGS) $(LIBS)

# ===== 編譯 client =====
$(BINDIR)/client: $(SRCDIR)/client.c $(INCDIR)/common.h $(LIBDIR)/libutils.so
	$(CC) $(CFLAGS) -o $@ $(SRCDIR)/client.c $(LDFLAGS) $(LIBS)

# ===== 清理 =====
clean:
	rm -f $(SRCDIR)/*.o
	rm -f $(BINDIR)/server $(BINDIR)/client
	rm -f $(LIBDIR)/libutils.so
