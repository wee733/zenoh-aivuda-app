# Zenoh Bridge development

The [video routing guide](docs/video-routing.md) describes how the bridge negotiates camera subscriptions. Preset and serialized rule definitions live in `src/include/zenoh_bridge/protocol.hpp`. Receiver-side discovery is in `src/src/demand_ingress.cpp`.

Build with the ROS environment sourced. To verify demand propagation, install ROS Python support for `sensor_msgs` and `foxglove_msgs`, then run:

```sh
./scripts/pre_install.sh
ROS_AUTOMATIC_DISCOVERY_RANGE=LOCALHOST python3 tests/video_demand.py build/zenoh_bridge_node
```

The test starts two loopback Zenoh peers in separate ROS domains and checks subscriber counts, payload order, exact bytes and idle teardown for both camera message types. It also checks source removal, type inference for a silent namespaced publisher, custom Zenoh keys, ROS remapping and older peer status with a missing message package. Existing component tests and packaging commands are in the [project README](README.md).
