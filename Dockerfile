ARG BUILD_ARCH=amd64
FROM ghcr.io/home-assistant/${BUILD_ARCH}-base:3.21 AS build
RUN apk add --no-cache build-base cmake mosquitto-dev sqlite-dev protobuf protobuf-dev openssl-dev
WORKDIR /build
COPY CMakeLists.txt src/ ./
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j"$(nproc)"

FROM ghcr.io/home-assistant/${BUILD_ARCH}-base:3.21
RUN apk add --no-cache libmosquitto sqlite-libs openssl
COPY --from=build /build/build/meshat-monitor /usr/local/bin/meshat-monitor
COPY run.sh /run.sh
RUN chmod a+x /run.sh
VOLUME ["/data"]
EXPOSE 8099
CMD ["/run.sh"]