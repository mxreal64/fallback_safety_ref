CXX      := g++
CXXFLAGS := -std=c++23 -O3 -march=native -mtune=native \
            -ffast-math -fmodules-ts -flto=auto \
            -fomit-frame-pointer -fstrict-aliasing \
            -funroll-loops -fno-plt \
            -fdevirtualize-at-ltrans

# -s strips symbols instantly for smaller, slightly faster cache loading
LDFLAGS  := -flto=auto -O3 -s -ljemalloc

TARGET   := safety_app
OBJS     := fallback_safety.o main.o

.PHONY: all clean run

all: $(TARGET)

fallback_safety.o: fallback_safety.cppm
	$(CXX) $(CXXFLAGS) -c $< -o $@

main.o: main.cpp fallback_safety.o
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(TARGET): $(OBJS)
	$(CXX) $(OBJS) $(LDFLAGS) -o $@

clean:
	rm -f $(OBJS) $(TARGET)
	rm -rf gcm.cache

# Helper to automatically run with optimized allocator settings
run: $(TARGET)
	MALLOC_CONF="background_thread:true,metadata_thp:always,dirty_decay_ms:1000,muzzy_decay_ms:1000" ./$(TARGET)
