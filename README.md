# Meshtastic Monitor

Low-overhead C++ monitor for Meshtastic MQTT packets. By default, it subscribes to `msh/#` on `mqtt.meshat.se`, covering all observed Meshtastic topic variants, writes each binary packet to SQLite, removes expired rows, and serves a small dashboard without a web framework.

## Local build

Install the development packages for SQLite and Eclipse Mosquitto, then run:

```sh
sudo apt install ninja-build libsqlite3-dev libmosquitto-dev protobuf-compiler libprotobuf-dev libssl-dev
cmake --preset release
cmake --build --preset release
./build/release/meshat-monitor --database ./meshat-monitor.db --topic 'msh/#' --retention-days 3
```

Open `http://localhost:8099`. Use `--topic` to narrow the subscription, for example `msh/SE/2/json/#`.

The same local defaults are available through `./run_local.sh`. It uses `meshat-monitor.db` beside the script and supports `MTSCOPE_DATABASE`, `MTSCOPE_TOPIC`, `MTSCOPE_RETENTION_DAYS`, and `MTSCOPE_HTTP_PORT` environment overrides. Extra command-line options are passed to the monitor.

## Browser tests

The dashboard has Playwright end-to-end tests. Install a native Linux Node.js installation (or use WSL2; WSL1 and Windows `npm` paths are not supported), then run:

```sh
npm install
npx playwright install --with-deps chromium
npm run test:e2e
```

On Debian or Ubuntu, the browser dependencies can also be installed explicitly:

```sh
sudo apt update
sudo apt install libnspr4 libnss3 libatk1.0-0t64 libatk-bridge2.0-0t64 libgbm1 libxcomposite1 libxdamage1 libxfixes3 libxrandr2 libasound2t64
```

The tests start the locally built monitor on port `18099` with an isolated database. They cover the dashboard shell, packet API, and mobile layout without requiring live MQTT data.

## Runtime properties

- SQLite is configured with WAL and `synchronous=NORMAL` to keep writes inexpensive.
- Inserts and retention deletes are prepared statements, and the time index makes purging bounded by expired data.
- Only the latest 100 packets are queried by the dashboard; MQTT data is never retained in application memory.
- Topic metadata and common JSON fields (`type`, `$typeName`, `sender`, and `channel`) are parsed into SQLite columns. Binary or encrypted payloads remain stored and displayed as hexadecimal when they cannot be decoded.
- When built with OpenSSL, encrypted packets are tried against Meshtastic's public default channel key (the firmware's `AQ==` key alias). Decrypted text, position, node-info, and telemetry packets use the same normalized measurements path. Private channel keys and direct-message PKC decryption are not attempted.
- Decoded application data is also inserted into `measurements`, linked to the raw packet by `packet_id`. It currently normalizes text, position, node info, and telemetry fields such as battery, voltage, temperature, humidity, and pressure.
- The C++ build generates Meshtastic bindings from `/home/sandos/projs/protobufs`; override this location with `-DMESHTASTIC_PROTOBUF_DIR=/path/to/protobufs` when configuring elsewhere.
- The Home Assistant image needs the protobuf definitions inside its Docker build context. Vendor `/home/sandos/projs/protobufs` as `protobufs/` in this repository, or adjust the Docker build context before building the add-on; Docker cannot copy a sibling directory outside its context.
- Persistent storage is `/data/meshat-monitor.db`, suitable for a Home Assistant add-on data volume.

## Home Assistant add-on

This repository contains the required `config.yaml`, `Dockerfile`, and `run.sh`. Add the repository in Home Assistant's add-on store, install the add-on, then set its MQTT topic and retention days. The dashboard is exposed on port `8099`.