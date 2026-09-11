# libs/logs: a small level-based logging library (not chidb-specific
# code) - see ../../CLAUDE.md. TOP is passed in by the parent Makefile.

include $(TOP)/Makefile.config

CFLAGS += -g3 -Wall -std=gnu99 -ggdb -D_GNU_SOURCE -MMD -MP

SRCS = log.c
OBJS = $(SRCS:.c=.o)

.PHONY: all clean

all: liblog.a

liblog.a: $(OBJS)
	$(AR) rcs $@ $(OBJS)

clean:
	rm -f *.o *.d liblog.a

-include $(OBJS:.o=.d)
