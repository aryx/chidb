# libs/check: a small vendored reimplementation of the subset of the
# Check unit-testing API (https://libcheck.github.io/check/) that
# tests/ uses - see check.h and ../../CLAUDE.md's Tests section. TOP
# is passed in by the parent Makefile.

include $(TOP)/Makefile.config

CFLAGS += -g3 -Wall -std=gnu99 -ggdb -D_GNU_SOURCE -MMD -MP

SRCS = check.c
OBJS = $(SRCS:.c=.o)

.PHONY: all clean

all: libcheck.a

libcheck.a: $(OBJS)
	$(AR) rcs $@ $(OBJS)

clean:
	rm -f *.o *.d libcheck.a

-include $(OBJS:.o=.d)
