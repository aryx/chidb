# src/libchidb: pager, B-Tree, DBM and codegen/optimizer - the DBM
# layers described in ../../CLAUDE.md's architecture diagram.
# TOP is passed in by the parent Makefile.

include $(TOP)/Makefile.config

CFLAGS += -I$(TOP)/include -I$(TOP)/libs/simclist -I$(TOP)/libs/logs -g3 -Wall -std=gnu99 -ggdb -D_GNU_SOURCE -MMD -MP

SRCS = \
  api.c \
  util.c \
  btree.c \
  pager.c \
  record.c \
  dbm.c \
  dbm-file.c \
  dbm-ops.c \
  dbm-cursor.c \
  codegen.c \
  optimizer.c
OBJS = $(SRCS:.c=.o)

.PHONY: all clean

all: libchidb.a

libchidb.a: $(OBJS)
	$(AR) rcs $@ $(OBJS)

clean:
	rm -f *.o *.d libchidb.a

-include $(OBJS:.o=.d)
