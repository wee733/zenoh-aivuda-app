# Zenoh Bridge for AivudaOS / ACEswarm

Official **Eclipse Zenoh 1.8 C++** dynamic ROS 2 bridge packaged as an AivudaOS / ACEswarm App, featuring a built-in Web UI for real-time topic discovery, QoS & rate/bandwidth control, multi-agent remote management, full Zenoh 1.8 JSON5 & visual kernel configuration, and live diagnostics.

When upgrading an existing installation, update AivudaOS to a build with `x-ros-domain-migration` support before installing this package. The installer copies the effective domain into the new configuration.

The bridge uses `zenoh.ros_domain_id`. Its default of -1 inherits the launch domain.

Set `ros.extra_overlay_setup_scripts` to an ordered YAML list of local workspace setup files (for example, `["~/apriltag_ws/install/local_setup.bash"]`). The bridge sources them after the base ROS installation and the existing `~/ros2_ws` overlay on startup. A missing configured script is reported at startup.

## Key Features

- **Zero-Transcode ROS 2 ↔ Zenoh Bridge**: Uses `rclcpp::GenericSubscription` / `GenericPublisher` with raw CDR payload passthrough over Zenoh (`zenoh-cpp` / `zenoh-c` 1.8).
- **Dynamic Live Routing & Flow Control**:
  - Per-topic **Egress** (ROS 2 → Zenoh) and wildcard **Ingress** (Zenoh → ROS 2) rules with hot-reload (no restart required) and YAML persistence (`routes.yaml`).
  - Frequency (`max_freq`), bandwidth (`max_kbps`), on-demand zero-idle publishing (`MatchingListener`), all 7 Zenoh priorities (`real_time` → `background`), congestion control (`drop` / `block` / `block_first`), `is_express`, reliability (`reliable` / `best_effort`), and **Locality** (`any` / `remote` / `session_local`).
- **Swarm Multi-Agent Remote Control**:
  - Automatic peer discovery across the swarm; inspect topics, view live logs, push presets, and hot-update both routing rules and Zenoh kernel configs on any online remote node via Zenoh Queryable RPC.
- **Full Official Zenoh 1.8.0 Configuration in Web UI**:
  - Minimal AivudaOS Config Panel (only `sys` magnet params & `ros` bootstrap environment); all Zenoh settings live in the Web UI.
  - **Dual Mode**: Grouped visual forms covering Mode/Identity/Endpoints, Scouting (Multicast & Gossip), Transport/MultiLink/TX-RX Queue/TCP tuning, SHM, TLS/mTLS/UsrPwd/PubKey/ACL, Routing/Timestamping/AdminSpace/Downsampling/LowPassFilter/QoS/Plugins — plus a **native full JSON5 editor** (`zenoh::Config::from_str`) with dry-run validation and import/export.
- **Live Diagnostics & Zenoh Lab**:
  - Real-time **Transports & Active Links** inspector (`get_transports()` / `get_links()` + connect/disconnect event logs).
  - **Active Scouting** (`zenoh::scout()`), **Liveliness** token monitor & custom token declare/undeclare, **PUT / DELETE / GET**, **AdminSpace (`@/**`)** quick queries, and **Live Key Sniffer**.

The `vision_debug` preset can stream both camera H.264 topics to a ground-station ROS reader on demand. See the [video routing guide](docs/video-routing.md).

## Configuration transfer and telemetry (2.3.0)

In **路由与流控**, select a node to export its configuration. On the target node, choose a JSON file, select sections and preview the route counts before applying. `GET /api/config/export?agent_id=<id>` returns format version 1. `POST /api/config/import` accepts `{config, mode, sections, dry_run, include_agent_id}`. The section names are `routes`, `bridge_settings` and `session_override`. Merge updates egress by topic and ingress by pattern. Replace removes rules missing from the selected route section. The import preserves the firewall policy and defaults to preserving local identity. Configuration files are journaled and replaced atomically. Committed routes reload automatically.

The browser gateway exposes `/zenoh/api/zenoh/stream?key_expr=*/heartbeat,*/comm_status,*/telemetry/*`. Each SSE message is a JSON object containing `key_expr` and `payload`. Use one EventSource for a view and close it on unmount. The direct loopback server exposes the same stream under `/api/zenoh/stream`. AivudaOS parameters `zenoh.heartbeat_enabled` and `zenoh.comm_status_enabled` independently enable 2 Hz presence reports. Disable the bridge heartbeat when a telemetry process owns that identity.

## DDS Shield: scope and administration

The shield is **off by default**. It filters host input/output on non-loopback interfaces using an `inet` nftables table, for both IPv4 and IPv6. `domain` filters the standard UDP destination-port ranges for the selected ROS domains; `all` additionally filters standard RTPS multicast discovery/data ports across domains. Domain IDs must be decimal 0–232, separated by commas; an empty setting uses the node's `ROS_DOMAIN_ID` (0 if unset). Invalid input is rejected, not replaced with domain 0.

This is **port filtering, not a DDS protocol firewall**. It may block other applications using the same UDP ports. Loopback and default Zenoh scouting to `224.0.0.224:7446/udp` are exempt; other traffic is not exempt merely because it uses port 7446 or 7447. Custom transports, nonstandard ports and forwarded packets are outside its scope. Configure non-conflicting Zenoh endpoints if needed.

- **Administration:** open 路由与流控 on the local node. A bridge bound to loopback accepts local policy edits without a token. Through Caddy, the original client address must match an active interface on the same machine. The gateway may be opened by local IP, system hostname, or the mDNS name from `AIVUDA_OS_AVAHI_HOSTNAME`. Setting `ZENOH_BRIDGE_SHIELD_TOKEN` in the bridge service environment requires the matching `X-DDS-Shield-Token` header. The UI reports the effective mode and prompts for the token when required. Remote policy management is disabled.
- **Privileges:** enabling the shield requires Linux nftables with JSON transactions, `inet` hooks and table handles, plus permission to administer the target network namespace. Run only a trusted service/install; do not grant general firewall privileges to an untrusted user. A persisted enabled policy must install before ROS initializes or the node will not start.
- **Lifecycle:** each installation manages a table derived from its absolute bridge settings file path. Keep this path stable. Stopping the process does not remove its filters; to remove them, explicitly disable the shield on the owning installation **before moving its data directory or uninstalling it**. Status only covers that installation's table, not other filters in the namespace. Saving bridge settings syncs both the replacement file and its parent directory before reporting success. If a directory sync fails, the new file may already be visible; inspect the settings and retry the save rather than assuming it survived a reboot.
- **Upgrading:** old `ZENOH_DDS_SHIELD_IN`/`ZENOH_DDS_SHIELD_OUT` iptables chains have no reliable owner. The new helper neither adopts nor deletes them. Have the host administrator review and retire legacy rules separately, after verifying the replacement on the target system.

## Directory Structure

```text
.
├── manifest.yaml              # AivudaOS app manifest
├── start.sh                   # Runtime bootstrap (ROS 2 env + bridge launch)
├── pack.sh                    # Schema validator & AivudaOS package builder
├── scripts/pre_install.sh     # CMake release build for zenoh_bridge_node
├── config/
│   ├── default_config.yaml    # Minimal sys + ros defaults
│   ├── config_schema.yaml     # AivudaOS config JSON Schema
│   └── caddy/zenoh_bridge.caddy
├── src/                       # C++17 rclcpp + zenoh-cpp bridge & HTTP API
└── ui/                        # Built-in Web UI (vanilla HTML/CSS/JS)
```

## Build & Package

Requires ROS 2 (e.g. Jazzy / Lyrical / Kilted / Humble) with `zenoh_cpp_vendor`, `yaml-cpp`, `nlohmann-json`, and `cmake`.

```bash
# Build native C++ binary
./scripts/pre_install.sh

# With ROS 2 sourced in this shell, test the local HTTP and Zenoh paths.
# Starts a temporary node on loopback with discovery disabled.
python3 tests/bridge_smoke.py build/zenoh_bridge_node

# Shield regressions: these use mock nft/helper implementations, not the host firewall.
python3 tests/test_dds_shield.py
python3 tests/test_config_schema.py
node --test tests/shield_ui.cjs
python3 tests/dds_shield_startup.py build/zenoh_bridge_node
python3 tests/shield_api.py build/zenoh_bridge_node
python3 tests/config_roundtrip.py build/zenoh_bridge_node
python3 tests/presence_stream.py build/zenoh_bridge_node

# Linux: verify durable file replacement, including simulated fsync errors.
c++ -std=c++17 -Isrc/include tests/atomic_write.cpp -Wl,--wrap=fsync -o build/test-atomic
build/test-atomic

# Optional rolling-upgrade test using a separately built, original v2.1 binary.
python3 tests/mixed_version.py build/zenoh_bridge_node /path/to/v2.1/zenoh_bridge_node

# Validate config schema and create AivudaOS .tar.gz archive in dist/
./pack.sh
# Match an existing AivudaOS AppStore installation ID when upgrading.
AIVUDA_PACKAGE_APP_ID=app_zenoh_2026042116_ba01 ./pack.sh
```
