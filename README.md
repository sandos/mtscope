# Meshtastic Monitor

Low-overhead monitor for Meshtastic MQTT traffic. It stores packets in SQLite and serves a dashboard for browsing packets, nodes, and measurements.

## Quick start

```sh
git clone --recurse-submodules https://github.com/sandos/mtscope.git
cd mtscope
./run_local.sh
```

Open <http://localhost:8099>. The default MQTT topic is `msh/#`.

Useful environment overrides:

```sh
MTSCOPE_TOPIC='msh/SE/2/json/#' ./run_local.sh
MTSCOPE_DATABASE=./monitor.db MTSCOPE_HTTP_PORT=8099 ./run_local.sh
```

## Home Assistant

Add this repository to Home Assistant's add-on store, install the Meshtastic Monitor add-on, and configure its MQTT topic and retention period. The dashboard is exposed on port `8099`.

## Development

Install the native dependencies:

```sh
sudo apt install cmake ninja-build g++ libsqlite3-dev libmosquitto-dev \
  protobuf-compiler libprotobuf-dev libssl-dev valgrind
```

Build and run the normal binary:

```sh
cmake --preset release
cmake --build --preset release
./build/release/meshat-monitor --database ./meshat-monitor.db --topic 'msh/#' --retention-days 3
```

Run the complete verification, including C++ tests and Playwright tests under both ASan/UBSan and Valgrind:

```sh
npm ci
npx playwright install --with-deps chromium
./verify.sh
```

The individual configurations are also available:

```sh
cmake --preset asan
cmake --build --preset asan --target asan

cmake --preset valgrind
cmake --build --preset valgrind --target valgrind

cmake --preset coverage
cmake --build --preset coverage --target meshat-monitor-tests
ctest --test-dir build/coverage --output-on-failure
```

To run the dashboard locally with the ASan monitor:

```sh
cmake --build --preset asan --target meshat-monitor
MTSCOPE_ASAN=1 ./run_local.sh
```

The protobuf definitions are included as the `protobufs/` submodule. Override their location with `-DMESHTASTIC_PROTOBUF_DIR=/path/to/protobufs` when configuring another checkout.

## Runtime notes

- SQLite uses WAL mode and retains packet history according to the configured retention period.
- The monitor separates logical packets from gateway observations, so repeated observations can be grouped without losing receiver metadata.
- JSON and protobuf payloads are normalized into measurements for text, position, node info, and telemetry data.
- Encrypted packets can be decoded with Meshtastic's public default channel key when OpenSSL is available.
- The standalone `mqtt-probe` checks broker connectivity without packet decoding:

  ```sh
  MQTT_HOST=mqtt.meshat.se MQTT_PORT=1883 MQTT_TOPIC='msh/#' \
    ./build/release/mqtt-probe
  ```

The SQLite schema is fresh-install-only. Remove the database files before restarting after schema changes:

```sh
rm -f meshat-monitor.db meshat-monitor.db-shm meshat-monitor.db-wal
```
