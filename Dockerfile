###############################################################################
# Overview
###############################################################################
# Build and test chidb (plain configure + Makefiles, C) on Ubuntu.
# See also .github/workflows/docker.yml for its use in Github Actions (GHA).

FROM ubuntu:24.04

# Setup a basic C dev environment. pkg-config is optional (./configure
# falls back to a plain link test for check if it's missing) but gives
# the most reliable detection, so keep it.
RUN apt-get update # needed otherwise can't find any package
RUN apt-get install -y --no-install-recommends \
      build-essential pkg-config

# chidb-specific deps: flex/bison for the SQL parser (sql.l/sql.y),
# and check for the test suite (./configure only warns and disables
# tests if check is missing, so install it to actually exercise
# `make check`)
RUN apt-get install -y --no-install-recommends \
      flex bison check

WORKDIR /src
COPY . .

RUN ./configure
RUN make

# Test
RUN make check

# Smoke test: run one of the demos/ scripts through the built shell
RUN ./chidb /tmp/library.cdb < demos/library.sql
