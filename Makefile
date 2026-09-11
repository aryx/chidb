# Top-level Makefile for chidb.
#
# Each src/<name>/ and tests/ is a self-contained piece with its own
# Makefile; this one just drives them, in dependency order, with the
# toolchain ./configure detected (Makefile.config, generated - do not
# edit by hand, re-run ./configure instead). Replaces the old
# autoconf/automake/libtool build (see changes.txt for when/why).
#
# Build layout: src/simclist, src/libcheck and src/libchisql are
# independent leaves; src/libchidb links against src/simclist and
# src/libchisql; src/shell links against all three and produces
# ./chidb at the repo root (see src/shell/Makefile). tests/ links
# against the same three libraries plus src/libcheck (a small vendored
# reimplementation of the Check unit-testing API - see
# src/libcheck/check.h).

include Makefile.config

SUBDIRS = src/simclist src/libcheck src/libchisql src/libchidb src/shell

.PHONY: all clean check test build-docker visual $(SUBDIRS)

###############################################################################
# Main targets
###############################################################################

all: $(SUBDIRS)

$(SUBDIRS):
	$(MAKE) -C $@ TOP=$(CURDIR)

src/libchidb: src/simclist src/libchisql
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
