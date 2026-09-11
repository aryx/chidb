# tests: the test suite (check_btree, check_dbrecord, check_dbm,
# check_pager, check_utils - see ../CLAUDE.md's Tests section), built
# against src/libcheck, a small vendored reimplementation of the Check
# unit-testing API (see ../src/libcheck/check.h) - no external test
# library dependency. TOP is passed in by the parent Makefile.

include $(TOP)/Makefile.config

CFLAGS += -I$(TOP)/include -I$(TOP)/src -I$(TOP)/src/simclist -I$(TOP)/src/libcheck -g3 -Wall -std=gnu99 -ggdb -D_GNU_SOURCE -MMD -MP
CFLAGS += -DTEST_DIR="\"$(TOP)/tests/\""

LIBCHIDB = $(TOP)/src/libchidb/libchidb.a
LIBCHISQL = $(TOP)/src/libchisql/libchisql.a
LIBSIMCLIST = $(TOP)/src/simclist/libsimclist.a
LIBCHECK = $(TOP)/src/libcheck/libcheck.a
LIBS = $(LIBCHIDB) $(LIBCHISQL) $(LIBSIMCLIST) $(LIBCHECK)

PROGS = check_btree check_dbrecord check_dbm check_pager check_utils

CHECK_BTREE_OBJS = check_btree.o check_btree_common.o check_btree_files.o \
                   check_btree_1a.o check_btree_1b.o check_btree_2.o \
                   check_btree_3.o check_btree_4.o check_btree_5.o \
                   check_btree_6.o check_btree_7.o check_btree_8.o \
                   check_common.o
CHECK_DBRECORD_OBJS = check_dbrecord.o
CHECK_DBM_OBJS = check_dbm.o check_common.o
CHECK_PAGER_OBJS = check_pager.o check_common.o
CHECK_UTILS_OBJS = check_utils.o

ALL_OBJS = $(CHECK_BTREE_OBJS) $(CHECK_DBRECORD_OBJS) $(CHECK_DBM_OBJS) \
           $(CHECK_PAGER_OBJS) $(CHECK_UTILS_OBJS)

.PHONY: all clean check test

all: $(PROGS)

check_btree: $(CHECK_BTREE_OBJS) $(LIBS)
	$(CC) -o $@ $(CHECK_BTREE_OBJS) $(LIBS)

check_dbrecord: $(CHECK_DBRECORD_OBJS) $(LIBS)
	$(CC) -o $@ $(CHECK_DBRECORD_OBJS) $(LIBS)

check_dbm: $(CHECK_DBM_OBJS) $(LIBS)
	$(CC) -o $@ $(CHECK_DBM_OBJS) $(LIBS)

check_pager: $(CHECK_PAGER_OBJS) $(LIBS)
	$(CC) -o $@ $(CHECK_PAGER_OBJS) $(LIBS)

check_utils: $(CHECK_UTILS_OBJS) $(LIBS)
	$(CC) -o $@ $(CHECK_UTILS_OBJS) $(LIBS)

check test: all
	set -e; for t in $(PROGS); do echo "=== $$t ==="; ./$$t; done

clean:
	rm -f *.o *.d $(PROGS)

-include $(ALL_OBJS:.o=.d)
