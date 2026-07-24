FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# Unlike the Python app's Dockerfile (which builds GNU Radio from source), Ubuntu 24.04's
# apt repo ships a recent-enough gnuradio-dev package with the C++ headers/libs we need -
# no source build required, which makes this image much faster to build.
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake pkg-config \
    gnuradio-dev \
    libvolk-bin \
    libboost-dev libboost-system-dev \
    libsqlite3-dev \
    nlohmann-json3-dev \
    portaudio19-dev \
    libsoapysdr-dev soapysdr-module-all soapysdr-module-rtlsdr \
    rtl-sdr librtlsdr-dev \
    libyaml-cpp-dev \
    libmp3lame-dev \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY . .

RUN cmake -B build -DCMAKE_BUILD_TYPE=Release . \
    && cmake --build build -j"$(nproc)" --target sdrscan sdrscan_import_yaml

# Config database lives on a mounted volume so it survives container recreation - this is
# what makes settings changes durable across restarts (see native/README.md).
VOLUME ["/data"]
EXPOSE 8080 8123

CMD ["./build/sdrscan", "-d", "/data/sdrscan.db", "-w", "web"]
