###############################################################################
# Overview
###############################################################################
# Build and test chidb (autotools, C) on Ubuntu.
# See also .github/workflows/docker.yml for its use in Github Actions (GHA).

FROM ubuntu:24.04

# Setup a basic C dev environment, plus the autotools chidb's own
# ./autogen.sh needs (configure/Makefile.in are gitignored, generated
# on demand -- see .gitignore's own comment there)
RUN apt-get update # needed otherwise can't find any package
RUN apt-get install -y --no-install-recommends \
      build-essential autoconf automake libtool pkg-config

# chidb-specific deps: flex/bison for the SQL parser (sql.l/sql.y),
# libedit for the interactive shell's line editing, and check for the
# test suite (configure.ac only warns and disables tests if check is
# missing, so install it to actually exercise `make check`)
RUN apt-get install -y --no-install-recommends \
      flex bison libedit-dev check

WORKDIR /src
COPY . .

RUN ./autogen.sh
RUN ./configure
RUN make

# Test
RUN make check

# Smoke test: run one of the demos/ scripts through the built shell
RUN ./chidb /tmp/library.cdb < demos/library.sql
