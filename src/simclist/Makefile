# src/simclist: vendored third-party generic C list library (not chidb
# code) - see ../../CLAUDE.md. TOP is passed in by the parent Makefile.

include $(TOP)/Makefile.config

CFLAGS += -I$(TOP)/include -g3 -Wall -std=gnu99 -ggdb -D_GNU_SOURCE -MMD -MP

SRCS = simclist.c
OBJS = $(SRCS:.c=.o)

.PHONY: all clean

all: libsimclist.a

libsimclist.a: $(OBJS)
	$(AR) rcs $@ $(OBJS)

clean:
	rm -f *.o *.d libsimclist.a

-include $(OBJS:.o=.d)
