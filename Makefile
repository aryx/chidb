# Top-level Makefile for chidb.
#
# Each src/<name>/, libs/<name>/ and tests/ is a self-contained piece
# with its own Makefile; this one just drives them, in dependency
# order, with the toolchain ./configure detected (Makefile.config,
# generated - do not edit by hand, re-run ./configure instead).
# Replaces the old autoconf/automake/libtool build (see changes.txt
# for when/why).
#
# Code organization: src/ is chidb-specific (the database itself);
# libs/ holds general-purpose, non-chidb-specific vendored library
# code (see changes.txt's "internals" entry for when/why).
#
# Build layout: libs/simclist, libs/check and src/libchisql are
# independent leaves; src/libchidb links against libs/simclist and
# src/libchisql; src/shell links against all three and produces
# ./chidb at the repo root (see src/shell/Makefile). tests/ links
# against the same three libraries plus libs/check (a small
# vendored reimplementation of the Check unit-testing API - see
# libs/check/check.h).

include Makefile.config

SUBDIRS = libs/simclist libs/check src/libchisql src/libchidb src/shell

.PHONY: all clean check test build-docker visual $(SUBDIRS)

###############################################################################
# Main targets
###############################################################################

all: $(SUBDIRS)

$(SUBDIRS):
	$(MAKE) -C $@ TOP=$(CURDIR)

src/libchidb: libs/simclist src/libchisql
src/shell: src/libchidb

check test: all
	$(MAKE) -C tests TOP=$(CURDIR) check

clean:
	for d in $(SUBDIRS) tests; do $(MAKE) -C $$d TOP=$(CURDIR) clean; done
	rm -f chidb

build-docker:
	docker build -t "chidb" .

###############################################################################
# Developer targets
###############################################################################

visual:
	codemap -screen_size 3 -efuns_client efuns_client -emacs_client /dev/null .
