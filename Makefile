CXX=g++
LD=g++
DEFS=-DFAT32_STRICT_MOUNT=1 -DFAT32_FSCK_REPAIR=1 -DFAT32_DATE_AND_TIME=1 \
	-DFAT32_LOCK_IMPL_H=\"fat32_posix_excl.h\" -DFAT32_LOCK_DEBUG=1

#	-DFAT32_LOCK_IMPL_H=\"fat32_no_excl.h\"


CXXFLAGS_DEBUG=-fno-inline -fno-exceptions -fno-rtti -fno-unwind-tables -ffunction-sections \
		-fdata-sections -g -O0 $(DEFS)

CXXFLAGS=-fno-exceptions -fno-rtti -fno-unwind-tables -ffunction-sections \
		-fdata-sections -Os $(DEFS)

LDFLAGS=-Wl,--gc-sections

SRCS=fat32.cxx cache.cxx gptmap.cxx test_fat32.cxx sdcard.cxx
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
	rm -f $(OBJS) test_fat32 $(DEPS)

-include $(DEPS)

.PHONY: test clean all valgrind
