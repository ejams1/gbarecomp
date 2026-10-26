# Build context is this directory: docker build -t gbarecomp-mgba-oracle:0.10.5 -f Dockerfile .
FROM debian:bookworm-slim@sha256:7ff6ba31527b9e336162fa061746c366c4b2032f4d2cff2078bea43f78efe3f8

ARG DEBIAN_SNAPSHOT=20261024T000000Z
ARG MGBA_COMMIT=26b7884bc25a5933960f3cdcd98bac1ae14d42e2

ENV DEBIAN_FRONTEND=noninteractive
RUN rm -f /etc/apt/sources.list.d/debian.sources \
    && printf 'deb [check-valid-until=no] http://snapshot.debian.org/archive/debian/%s bookworm main\ndeb [check-valid-until=no] http://snapshot.debian.org/archive/debian/%s bookworm-updates main\ndeb [check-valid-until=no] http://snapshot.debian.org/archive/debian-security/%s bookworm-security main\n' "$DEBIAN_SNAPSHOT" "$DEBIAN_SNAPSHOT" "$DEBIAN_SNAPSHOT" > /etc/apt/sources.list \
    && apt-get -o Acquire::Check-Valid-Until=false update \
    && apt-get install -y --no-install-recommends \
       ca-certificates git build-essential cmake ninja-build pkg-config \
       zlib1g-dev libpng-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /opt/gbarecomp
COPY . /opt/gbarecomp/oracle

# Linux needs no Windows compatibility patches; verify their tracked hashes.
RUN cd /opt/gbarecomp/oracle && sha256sum -c dependency-hashes.sha256

RUN git clone --filter=blob:none --no-checkout https://github.com/mgba-emu/mgba.git /opt/gbarecomp/third_party/mgba \
    && git -C /opt/gbarecomp/third_party/mgba checkout --detach "$MGBA_COMMIT" \
    && test "$(git -C /opt/gbarecomp/third_party/mgba rev-parse HEAD)" = "$MGBA_COMMIT"

RUN cmake -S /opt/gbarecomp/third_party/mgba -B /opt/gbarecomp/third_party/mgba/build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
      -DLIBMGBA_ONLY=ON -DBUILD_QT=OFF -DBUILD_SDL=OFF \
      -DUSE_FFMPEG=OFF -DUSE_LIBZIP=OFF -DUSE_LZMA=OFF \
      -DUSE_EPOXY=OFF -DUSE_DEBUGGERS=OFF -DBUILD_GL=OFF \
      -DBUILD_GLES2=OFF -DBUILD_GLES3=OFF \
    && cmake --build /opt/gbarecomp/third_party/mgba/build --parallel 2 \
    && test -f /opt/gbarecomp/third_party/mgba/build/libmgba.a

RUN cmake -S /opt/gbarecomp/oracle -B /opt/gbarecomp/build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release -DMGBA_ROOT=/opt/gbarecomp/third_party/mgba \
    && cmake --build /opt/gbarecomp/build --parallel 2

EXPOSE 19843/tcp
ENTRYPOINT ["/opt/gbarecomp/build/gbarecomp_oracle"]
CMD ["--no-bios", "--port", "19843"]
