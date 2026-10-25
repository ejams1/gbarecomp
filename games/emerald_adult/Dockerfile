FROM debian@sha256:7ff6ba31527b9e336162fa061746c366c4b2032f4d2cff2078bea43f78efe3f8

ENV DEBIAN_FRONTEND=noninteractive
RUN printf '%s\n' \
    'deb [check-valid-until=no] http://snapshot.debian.org/archive/debian/20261024T000000Z bookworm main' \
    'deb [check-valid-until=no] http://snapshot.debian.org/archive/debian-security/20261024T000000Z bookworm-security main' \
    > /etc/apt/sources.list \
 && rm -f /etc/apt/sources.list.d/debian.sources \
 && apt-get -o Acquire::Check-Valid-Until=false update \
 && apt-get install -y --no-install-recommends \
    build-essential ca-certificates clang cmake git ninja-build pkg-config \
 && rm -rf /var/lib/apt/lists/*

# SDL2 is built from the exact upstream release commit. The dummy video/audio
# drivers let CI verify HostWindow initialization and teardown without a GUI.
RUN git clone https://github.com/libsdl-org/SDL.git /tmp/SDL \
 && git -C /tmp/SDL checkout --detach fa24d868ac2f8fd558e4e914c9863411245db8fd \
 && test "$(git -C /tmp/SDL rev-parse HEAD)" = "fa24d868ac2f8fd558e4e914c9863411245db8fd" \
 && cmake -S /tmp/SDL -B /tmp/SDL/build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr/local \
    -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST=OFF -DSDL_TESTS=OFF \
    -DSDL_DUMMYVIDEO=ON -DSDL_DUMMYAUDIO=ON \
    -DSDL_X11=OFF -DSDL_WAYLAND=OFF -DSDL_KMSDRM=OFF \
 && cmake --build /tmp/SDL/build --parallel \
 && cmake --install /tmp/SDL/build \
 && rm -rf /tmp/SDL

WORKDIR /workspace
