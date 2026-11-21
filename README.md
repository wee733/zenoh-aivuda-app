# Zenoh Bridge for AivudaOS / ACEswarm

Official **Eclipse Zenoh 1.8 C++** dynamic ROS 2 bridge packaged as an AivudaOS / ACEswarm App, featuring a built-in Web UI for real-time topic discovery, QoS & rate/bandwidth control, multi-agent remote management, full Zenoh 1.8 JSON5 & visual kernel configuration, and live diagnostics.

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

# Validate config schema and create AivudaOS .tar.gz archive in dist/
./pack.sh
```
