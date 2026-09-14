# Linux build/test environment for the c-shell.
#
# GCC 14 is the first release that accepts -std=c23.  lsof, ps and script
# are only needed by the tests (lsof cross-checks spy; script gives the
# shell a real terminal).  Source is mounted at run time, not copied in,
# so the image never needs rebuilding when the code changes.
FROM gcc:14

RUN apt-get update \
 && apt-get install -y --no-install-recommends lsof procps bsdutils \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /build
