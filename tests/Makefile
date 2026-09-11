# tests: the Check-based test suite (check_btree, check_dbrecord,
# check_dbm, check_pager, check_utils - see ../CLAUDE.md's Tests
# section). Built only if ./configure found the Check library
# (HAS_CHECK); otherwise `all`/`check` are a no-op warning, matching
# the old configure.ac's PKG_CHECK_MODULES behavior. TOP is passed in
# by the parent Makefile.

include $(TOP)/Makefile.config

CFLAGS += -I$(TOP)/include -I$(TOP)/src -I$(TOP)/src/simclist -g3 -Wall -std=gnu99 -ggdb -D_GNU_SOURCE -MMD -MP
CFLAGS += -DTEST_DIR="\"$(TOP)/tests/\""
CFLAGS += $(CHECK_CFLAGS)

LIBCHIDB = $(TOP)/src/libchidb/libchidb.a
LIBCHISQL = $(TOP)/src/libchisql/libchisql.a
LIBSIMCLIST = $(TOP)/src/simclist/libsimclist.a
LIBS = $(LIBCHIDB) $(LIBCHISQL) $(LIBSIMCLIST)

ALL_PROGS = check_btree check_dbrecord check_dbm check_pager check_utils
ifeq ($(HAS_CHECK),yes)
PROGS = $(ALL_PROGS)
else
PROGS =
endif

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
	$(CC) -o $@ $(CHECK_BTREE_OBJS) $(LIBS) $(CHECK_LIBS)

check_dbrecord: $(CHECK_DBRECORD_OBJS) $(LIBS)
	$(CC) -o $@ $(CHECK_DBRECORD_OBJS) $(LIBS) $(CHECK_LIBS)

check_dbm: $(CHECK_DBM_OBJS) $(LIBS)
	$(CC) -o $@ $(CHECK_DBM_OBJS) $(LIBS) $(CHECK_LIBS)

check_pager: $(CHECK_PAGER_OBJS) $(LIBS)
	$(CC) -o $@ $(CHECK_PAGER_OBJS) $(LIBS) $(CHECK_LIBS)

check_utils: $(CHECK_UTILS_OBJS) $(LIBS)
	$(CC) -o $@ $(CHECK_UTILS_OBJS) $(LIBS) $(CHECK_LIBS)

check test: all
ifeq ($(HAS_CHECK),yes)
	set -e; for t in $(PROGS); do echo "=== $$t ==="; ./$$t; done
else
	@echo "check: the Check library was not found at configure time (see ./configure output)"
	@echo "check: skipping the test suite - built everything else fine"
endif

clean:
	rm -f *.o *.d $(ALL_PROGS)

-include $(ALL_OBJS:.o=.d)
