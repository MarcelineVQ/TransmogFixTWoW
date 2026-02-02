# Makefile for transmogfix.dll
# Standalone DLL fix for WoW 1.12.1 transmog death frame drops
#
# Build targets:
#   make            - Build standalone DLL (bundles MinHook)
#   make lib        - Build static library (for embedding in other DLLs)
#   make clean      - Clean all build artifacts
#   make release    - Build optimized & stripped release
#
# The Makefile automatically fetches the MinHook submodule if needed.

# Architecture: x86 (32-bit) - WoW 1.12 is 32-bit
CC      = i686-w64-mingw32-gcc
CXX     = i686-w64-mingw32-g++
AR      = i686-w64-mingw32-ar
STRIP   = i686-w64-mingw32-strip

# Directories
BUILD_DIR = build
MINHOOK_DIR = minhook

# Outputs
DLL_TARGET = transmogfix.dll
LIB_TARGET = libtransmogfix.a

# Source files
CORE_SRCS = transmogCoalesce.cpp
DLL_SRCS  = dllmain.cpp $(CORE_SRCS)

# MinHook source files
MINHOOK_SRCS = \
    $(MINHOOK_DIR)/src/buffer.c \
    $(MINHOOK_DIR)/src/hook.c \
    $(MINHOOK_DIR)/src/trampoline.c \
    $(MINHOOK_DIR)/src/hde/hde32.c

# Object files
CORE_OBJS    = $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(CORE_SRCS))
DLL_OBJS     = $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(DLL_SRCS))
MINHOOK_OBJS = $(BUILD_DIR)/minhook_buffer.o \
               $(BUILD_DIR)/minhook_hook.o \
               $(BUILD_DIR)/minhook_trampoline.o \
               $(BUILD_DIR)/minhook_hde32.o

# Compiler flags
COMMON_FLAGS = -DUNICODE -D_UNICODE -DWIN32 -D_WIN32
COMMON_FLAGS += -I$(MINHOOK_DIR)/include
COMMON_FLAGS += -Wall

DEBUG_FLAGS   = -g -O0 -DDEBUG
RELEASE_FLAGS = -O2 -DNDEBUG

CFLAGS   = $(COMMON_FLAGS) -std=c11
CXXFLAGS = $(COMMON_FLAGS) -std=c++17

# Linker flags
LDFLAGS  = -shared
LDFLAGS += -static -static-libgcc -static-libstdc++
LDFLAGS += -Wl,--subsystem,windows

# Build rules
.PHONY: all clean release lib dirs submodule check-submodule

all: debug

# Check if submodule needs initialization
check-submodule:
	@if [ ! -f "$(MINHOOK_DIR)/include/MinHook.h" ]; then \
		echo "MinHook submodule not found, initializing..."; \
		cd .. && git submodule update --init --recursive transmogfix/minhook; \
	fi

submodule: check-submodule

debug: CFLAGS += $(DEBUG_FLAGS)
debug: CXXFLAGS += $(DEBUG_FLAGS)
debug: submodule dirs $(DLL_OBJS) $(MINHOOK_OBJS)
	$(CXX) $(LDFLAGS) -o $(DLL_TARGET) $(DLL_OBJS) $(MINHOOK_OBJS)
	@echo "Built: $(DLL_TARGET)"
	@ls -lh $(DLL_TARGET) | awk '{print "Size: " $$5}'

release: CFLAGS += $(RELEASE_FLAGS)
release: CXXFLAGS += $(RELEASE_FLAGS)
release: submodule dirs $(DLL_OBJS) $(MINHOOK_OBJS)
	$(CXX) $(LDFLAGS) -o $(DLL_TARGET) $(DLL_OBJS) $(MINHOOK_OBJS)
	$(STRIP) --strip-all $(DLL_TARGET)
	@echo "Release built: $(DLL_TARGET)"
	@ls -lh $(DLL_TARGET) | awk '{print "Size: " $$5}'

# Static library for embedding (no MinHook, no dllmain)
lib: CXXFLAGS += $(RELEASE_FLAGS)
lib: dirs $(CORE_OBJS)
	$(AR) rcs $(LIB_TARGET) $(CORE_OBJS)
	@echo "Static library built: $(LIB_TARGET)"

dirs:
	@mkdir -p $(BUILD_DIR)

# C++ objects
$(BUILD_DIR)/%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# MinHook objects
$(BUILD_DIR)/minhook_buffer.o: $(MINHOOK_DIR)/src/buffer.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/minhook_hook.o: $(MINHOOK_DIR)/src/hook.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/minhook_trampoline.o: $(MINHOOK_DIR)/src/trampoline.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/minhook_hde32.o: $(MINHOOK_DIR)/src/hde/hde32.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf $(BUILD_DIR)
	rm -f $(DLL_TARGET) $(LIB_TARGET)
