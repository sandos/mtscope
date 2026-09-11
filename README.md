# Meshtastic Monitor

Low-overhead C++ monitor for Meshtastic MQTT packets. By default, it subscribes to `msh/#` on `mqtt.meshat.se`, covering all observed Meshtastic topic variants, writes each binary packet to SQLite, removes expired rows, and serves a small dashboard without a web framework.

## Local build

Install the development packages for SQLite and Eclipse Mosquitto, then run:

```sh
sudo apt install ninja-build libsqlite3-dev libmosquitto-dev protobuf-compiler libprotobuf-dev libssl-dev
git clone --recurse-submodules https://github.com/sandos/mtscope.git
cd mtscope
cmake --preset release
cmake --build --preset release
./build/release/meshat-monitor --database ./meshat-monitor.db --topic 'msh/#' --retention-days 3
```

To test the broker/client connection without SQLite, HTTP, or packet decoding, run the standalone probe:

```sh
MQTT_HOST=mqtt.meshat.se MQTT_PORT=1883 MQTT_TOPIC='msh/#' \
	./build/release/mqtt-probe
```

It accepts `host`, `port`, `topic`, and duration in seconds as positional arguments. Credentials are read from `MQTT_USERNAME` and `MQTT_PASSWORD`. Compare its disconnect count with the monitor over the same interval; a clean probe alongside disconnecting monitor points to work in the monitor's MQTT callback, especially the synchronous database insert.

The Meshtastic protobuf definitions are included as the `protobufs/` git submodule. For an existing checkout, initialize it with `git submodule update --init --recursive`.

Open `http://localhost:8099`. Use `--topic` to narrow the subscription, for example `msh/SE/2/json/#`.

The same local defaults are available through `./run_local.sh`. It uses `meshat-monitor.db` beside the script and supports `MTSCOPE_DATABASE`, `MTSCOPE_TOPIC`, `MTSCOPE_RETENTION_DAYS`, `MTSCOPE_HTTP_PORT`, and `MTSCOPE_WEB_ROOT` environment overrides. Extra command-line options are passed to the monitor.

The SQLite schema is intentionally fresh-install-only and is not migrated. When upgrading across schema changes, stop the monitor and remove the database before restarting:

```sh
rm -f meshat-monitor.db meshat-monitor.db-shm meshat-monitor.db-wal
```

This discards retained packet history and starts with the current schema.

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
- Only the latest 500 packets are queried by the dashboard; MQTT data is never retained in application memory.
- The monitor uses a unique MQTT client ID per process (`meshat-monitor-<pid>`). A fixed client ID causes brokers to disconnect an existing session whenever another monitor instance connects with the same ID.
- Topic metadata and common JSON fields (`type`, `$typeName`, `sender`, and `channel`) are parsed into SQLite columns. Binary or encrypted payloads remain stored and displayed as hexadecimal when they cannot be decoded.
- When built with OpenSSL, encrypted packets are tried against Meshtastic's public default channel key (the firmware's `AQ==` key alias). Decrypted text, position, node-info, and telemetry packets use the same normalized measurements path. Private channel keys and direct-message PKC decryption are not attempted.
- Decoded application data is also inserted into `measurements`, linked to `logical_packets`. It currently normalizes text, position, node info, and telemetry fields such as battery, voltage, temperature, humidity, and pressure.
- The database separates `logical_packets` from `observations`: one logical packet can have many MQTT observations, retaining each gateway and its receiver metadata. `measurements` remains historical and is linked to logical packets; node short names are resolved from the latest applicable node-info measurement rather than copied into a mutable node table.
- The C++ build generates Meshtastic bindings from the `protobufs/` submodule; override this location with `-DMESHTASTIC_PROTOBUF_DIR=/path/to/protobufs` when configuring elsewhere. A sibling `../protobufs` checkout is also accepted for local development.
- The Home Assistant image fetches the pinned public protobuf revision during its Docker build, so the add-on repository does not need to expose the submodule contents in the Home Assistant build context.
- Persistent storage is `/data/meshat-monitor.db`, suitable for a Home Assistant add-on data volume.

## Home Assistant add-on

This repository contains the required `config.yaml`, `Dockerfile`, and `run.sh`. Add the repository in Home Assistant's add-on store, install the add-on, then set its MQTT topic and retention days. The dashboard is exposed on port `8099`.