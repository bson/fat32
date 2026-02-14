CXX=g++
LD=g++
CXXFLAGS_DEBUG=-fno-inline -fno-exceptions -fno-rtti -fno-unwind-tables -ffunction-sections \
		-fdata-sections -g -O0

CXXFLAGS=-fno-exceptions -fno-rtti -fno-unwind-tables -ffunction-sections \
		-fdata-sections -Os

LDFLAGS=-Wl,--gc-sections

CXX_O=fat32.o cache.o test_fat32.o


all:	test_fat32

test: test_fat32
	./$^

valgrind: test_fat32
	valgrind $^

test_fat32:	$(CXX_O)
	$(LD) -o $@ $(LDFLAGS) $^

%.o:	%.cxx
	$(CXX) $(CXXFLAGS_DEBUG) -o $@ -c $<


clean:
	rm -f $(CXX_O) test_fat32

.PHONY: test clean all valgrind
