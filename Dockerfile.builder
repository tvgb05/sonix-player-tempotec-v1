FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential git pkg-config wget curl ca-certificates \
    texinfo bison flex gawk gperf patch xz-utils bzip2 gzip \
    autoconf automake libtool libtool-bin \
    libgmp-dev libmpfr-dev libmpc-dev \
    p7zip-full squashfs-tools genisoimage python3 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace
CMD ["/bin/bash", "/workspace/build_local.sh"]
