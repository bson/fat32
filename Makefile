CXX=g++
LD=g++
CXXFLAGS_DEBUG=-fno-inline -fno-exceptions -fno-rtti -fno-unwind-tables -ffunction-sections \
		-fdata-sections -g -O0

CXXFLAGS=-fno-exceptions -fno-rtti -fno-unwind-tables -ffunction-sections \
		-fdata-sections -Os

LDFLAGS=-Wl,--gc-sections

SRCS=fat32.cxx cache.cxx test_fat32.cxx
OBJS=$(patsubst %.cxx, %.o, $(SRCS))
DEPS=$(patsubst %.cxx, %.d, $(SRCS))

DEPFLAGS = -MM

all:	test_fat32

test: test_fat32
	./$^

valgrind: test_fat32
	valgrind $^

test_fat32:	$(OBJS) $(DEPS)
	$(LD) -o $@ $(LDFLAGS) $(OBJS)

%.o : %.cxx
	$(CXX) $(CXXFLAGS_DEBUG) -o $@ -c $<

%.d : %.cxx
	@$(CXX) $(CXXFLAGS_DEBUG) $(DEPFLAGS) -o $@ -c $<

clean:
	rm -f $(OBJS) test_fat32

-include $(DEPS)

.PHONY: test clean all valgrind
