# TEST/Makefile - fs_aging host testleri
#
# TEST klasorunde (ya da depo kokunde "make -C TEST") calistirilir:
#   make        : tum test programlarini build/ altina derler
#   make test   : edge testlerini ve 8 profilin host testini kosar;
#                 herhangi bir test basarisizsa sifirdan farkli doner
#   make clean  : build/ klasorunu siler
#
# MSYS2 make (sh ile) ve mingw32-make (sh yoksa cmd.exe ile) desteklenir.
# Ayrintili tek kosu ornegi:
#   build/host_test agrawal 268435456 0 42

ifeq ($(origin CC),default)
CC := gcc
endif

CFLAGS   := -std=c99 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -O2
CPPFLAGS := -I../CORE/include

CORE_SRC := ../CORE/src/fs_aging.c ../CORE/src/fs_aging_profiles.c
CORE_HDR := ../CORE/include/fs_aging.h ../CORE/include/fs_aging_profiles.h

ifeq ($(OS),Windows_NT)
EXE := .exe
else
EXE :=
endif

# Tarif kabugu: cmd.exe 'echo "x"' ciktisinda tirnaklari korur, sh korumaz.
ifeq ($(shell echo "x"),"x")
RUN       := $(subst /,\,build/)
MKDIR_CMD := if not exist build mkdir build
RM_CMD    := if exist build rmdir /s /q build
else
RUN       := build/
MKDIR_CMD := mkdir -p build
RM_CMD    := rm -rf build
endif

HOST_TEST  := build/host_test$(EXE)
EDGE_TEST  := build/edge_tests$(EXE)
EDGE_PATH  := build/edge_tests_path64$(EXE)

.PHONY: all test clean

all: $(HOST_TEST) $(EDGE_TEST) $(EDGE_PATH)

build:
	$(MKDIR_CMD)

$(HOST_TEST): src/host_test.c $(CORE_SRC) $(CORE_HDR) | build
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $(CORE_SRC) src/host_test.c

$(EDGE_TEST): src/edge_tests.c $(CORE_SRC) $(CORE_HDR) | build
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $(CORE_SRC) src/edge_tests.c

# Core ve test ayni FS_AGING_PATH_MAX ile derlenmeli (context yapisi buna bagli)
$(EDGE_PATH): src/edge_tests.c $(CORE_SRC) $(CORE_HDR) | build
	$(CC) $(CFLAGS) $(CPPFLAGS) -DFS_AGING_PATH_MAX=64u -o $@ $(CORE_SRC) src/edge_tests.c

# Profil kosulari: seed 42, confidence 0 (tam yakinsama). Volume'ler her
# profilin onerilen havuzla (tahmin + %25) DONE_* durumuna ulastigi ve
# toplam surenin kisa kaldigi degerlerdir.
# Kalite kapisi: agrawal 256 MB onceki dogrulanmis sonucla ayni olmalidir
# (islem=155682, yas_hatasi=0.0023); diger profillerde yas/boyut/dizin
# hatasi (toplam varyasyon mesafesi) MAX_ERR'i asmamalidir.
# Not: bu volume/seed secimleri tohuma baglidir. Ozellikle douceur 64 MB
# baska seed'lerle ERR_PLATFORM (STABLE'da canli dosya 0'a iner) ya da
# ERR_POOL_FULL (havuz tahmini + %25 yetmez) ile bitebilir; core bu iki
# konuda kullanici karari bekliyor (README bolum 3 ve 5).
AGRAWAL_OPS := 155682
AGRAWAL_ERR := 0.0025
MAX_ERR     := 0.15

test: all
	$(RUN)edge_tests$(EXE)
	$(RUN)edge_tests_path64$(EXE)
	$(RUN)host_test$(EXE) -q --expect-ops $(AGRAWAL_OPS) --max-err $(AGRAWAL_ERR) agrawal 268435456 0 42
	$(RUN)host_test$(EXE) -q --max-err $(MAX_ERR) dabre     4294967296  0 42
	$(RUN)host_test$(EXE) -q --max-err $(MAX_ERR) douceur   67108864    0 42
	$(RUN)host_test$(EXE) -q --max-err $(MAX_ERR) grundman  1073741824  0 42
	$(RUN)host_test$(EXE) -q --max-err $(MAX_ERR) meyer     1073741824  0 42
	$(RUN)host_test$(EXE) -q --max-err $(MAX_ERR) pramod    17179869184 0 42
	$(RUN)host_test$(EXE) -q --max-err $(MAX_ERR) wang_lanl 268435456   0 42
	$(RUN)host_test$(EXE) -q --max-err $(MAX_ERR) wang_os   268435456   0 42

clean:
	$(RM_CMD)
