FROM ubuntu:22.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
make gcc-arm-none-eabi binutils-arm-none-eabi \
libnewlib-arm-none-eabi libstdc++-arm-none-eabi-newlib \
python3 git ca-certificates srecord \
&& rm -rf /var/lib/apt/lists/*
WORKDIR /workspace
