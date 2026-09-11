###############################################################################
# Overview
###############################################################################
# Build and test chidb (plain configure + Makefiles, C) on Ubuntu.
# See also .github/workflows/docker.yml for its use in Github Actions (GHA).

FROM ubuntu:24.04

# Setup a basic C dev environment.
RUN apt-get update # needed otherwise can't find any package
RUN apt-get install -y --no-install-recommends \
      build-essential

# chidb-specific deps: flex/bison for the SQL parser (sql.l/sql.y).
# The test suite (src/libcheck) is vendored, so no separate package is
# needed to exercise `make check`.
RUN apt-get install -y --no-install-recommends \
      flex bison

WORKDIR /src
COPY . .

RUN ./configure
RUN make

# Test
RUN make check

# Smoke test: run one of the demos/ scripts through the built shell
RUN ./chidb /tmp/library.cdb < demos/library.sql
