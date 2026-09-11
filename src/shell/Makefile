# src/shell: the interactive `chidb` REPL binary (libedit-based), built
# on top of libchidb - not a layer other code depends on. Links the
# final binary directly to $(TOP)/chidb, so `./chidb` works from the
# repo root with no install step. TOP is passed in by the parent
# Makefile.

include $(TOP)/Makefile.config

CFLAGS += -I$(TOP)/include -g3 -Wall -std=gnu99 -ggdb -D_GNU_SOURCE -MMD -MP
CFLAGS += $(LIBEDIT_CFLAGS)

SRCS = main.c shell.c commands.c
OBJS = $(SRCS:.c=.o)

LIBCHIDB = $(TOP)/src/libchidb/libchidb.a
LIBCHISQL = $(TOP)/src/libchisql/libchisql.a
LIBSIMCLIST = $(TOP)/src/simclist/libsimclist.a

.PHONY: all clean

all: $(TOP)/chidb

$(TOP)/chidb: $(OBJS) $(LIBCHIDB) $(LIBCHISQL) $(LIBSIMCLIST)
	$(CC) -o $@ $(OBJS) $(LIBCHIDB) $(LIBCHISQL) $(LIBSIMCLIST) $(LIBEDIT_LIBS)

clean:
	rm -f *.o *.d

-include $(OBJS:.o=.d)
