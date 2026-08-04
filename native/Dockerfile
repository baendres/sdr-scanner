FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# Unlike the Python app's Dockerfile (which builds GNU Radio from source), Ubuntu 24.04's
# apt repo ships a recent-enough gnuradio-dev package with the C++ headers/libs we need -
# no source build required, which makes this image much faster to build.
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake pkg-config git \
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

# mbelib + DSDcc: neither is packaged in Ubuntu 24.04's apt repo. DSDcc (f4exb/dsdcc) wraps
# mbelib (f4exb/mbelib) to add DMR/P25 Phase 1 frame sync/deframing on top of its AMBE/IMBE
# decode - the same reverse-engineered codec family OpenWebRX+'s digiham and SDRTrunk's jmbe
# use, run entirely in software (no AMBE hardware dongle). See native/README.md for the
# DSDDecoder::getOpts()/getState() vendored-patch caveat if bumping either dependency's version.
# dsdcc-diagnostics.patch exposes DSDDecoder's internal per-pattern DMR sync mismatch counts
# (getDmrDataBsSyncErrors() etc.) - see native/README.md's DMR sync-type diagnostics note.
COPY dsdcc-diagnostics.patch /tmp/dsdcc-diagnostics.patch
RUN git clone --depth 1 https://github.com/f4exb/mbelib.git /tmp/mbelib \
    && cmake -S /tmp/mbelib -B /tmp/mbelib/build -DCMAKE_BUILD_TYPE=Release -DDISABLE_TEST=ON \
    && cmake --build /tmp/mbelib/build -j"$(nproc)" \
    && cmake --install /tmp/mbelib/build \
    && git clone --depth 1 https://github.com/f4exb/dsdcc.git /tmp/dsdcc \
    && patch -p1 -d /tmp/dsdcc < /tmp/dsdcc-diagnostics.patch \
    && cmake -S /tmp/dsdcc -B /tmp/dsdcc/build -DCMAKE_BUILD_TYPE=Release -DUSE_MBELIB=ON -DBUILD_TOOL=OFF \
    && cmake --build /tmp/dsdcc/build -j"$(nproc)" \
    && cmake --install /tmp/dsdcc/build \
    && ldconfig \
    && rm -rf /tmp/mbelib /tmp/dsdcc /tmp/dsdcc-diagnostics.patch

WORKDIR /app
COPY . .

RUN cmake -B build -DCMAKE_BUILD_TYPE=Release . \
    && cmake --build build -j"$(nproc)" --target sdrscan sdrscan_import_yaml

# Config database lives on a mounted volume so it survives container recreation - this is
# what makes settings changes durable across restarts (see native/README.md).
VOLUME ["/data"]
EXPOSE 8080 8123

CMD ["./build/sdrscan", "-d", "/data/sdrscan.db", "-w", "web"]
