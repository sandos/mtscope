ARG BUILD_ARCH=amd64
FROM ghcr.io/home-assistant/base:latest AS build
ARG MESHTASTIC_PROTOBUF_REF=a9e83e98b8d79e757f2985bfbcb4d00c7f630157
RUN apk add --no-cache build-base cmake git mosquitto-dev sqlite-dev protobuf protobuf-dev openssl-dev
WORKDIR /build
COPY CMakeLists.txt ./
COPY src ./src
RUN git clone https://github.com/meshtastic/protobufs.git protobufs \
	&& git -C protobufs checkout "${MESHTASTIC_PROTOBUF_REF}" \
	&& cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DMESHTASTIC_PROTOBUF_DIR=/build/protobufs \
	&& cmake --build build -j"$(nproc)"

FROM ghcr.io/home-assistant/base:latest
RUN apk add --no-cache mosquitto-libs sqlite-libs openssl
COPY --from=build /build/build/meshat-monitor /usr/local/bin/meshat-monitor
COPY web /usr/local/share/meshat-monitor
COPY run.sh /run.sh
RUN chmod a+x /run.sh
VOLUME ["/data"]
EXPOSE 8099
CMD ["/run.sh"]