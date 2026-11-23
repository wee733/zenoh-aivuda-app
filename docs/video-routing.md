# IMX219 H.264 video routing

Apply the `vision_debug` preset on the two bridge instances for a fresh configuration. If the node already has custom routes, merge the two camera rules from the preset into the current rules. Applying a whole preset replaces the current rule set.

| Airborne ROS topic | Message type | Ground topic for robot `uav1` |
| --- | --- | --- |
| `/camera/video/h264` | `foxglove_msgs/msg/CompressedVideo` | `/uav1/camera/video/h264` |
| `/camera/image_compressed` | `sensor_msgs/msg/CompressedImage` | `/uav1/camera/image_compressed` |

The camera selects which topics it publishes. Install the message type support in the ROS environments used by both bridges. Connect Foxglove to the ground station's ROS graph through its Foxglove bridge. In an Image panel select the robot's `camera/video/h264` topic. The encoded stream is Annex B H.264. See the [Foxglove schema](https://docs.foxglove.dev/docs/sdk/schemas/compressed-video).

The source uses `on_demand: true`, `max_freq: 0`, `max_kbps: 0`, best-effort ROS and Zenoh delivery, remote locality, data-low priority and drop-on-congestion. The receiver uses `on_demand: true`, `max_freq: 0` and best-effort ROS delivery. Adjust the frame rate in the camera app. An enabled unconditionally subscribed wildcard ingress or Zenoh sniffer covering the video key also counts as a reader.

In 2.4.0 the ground bridge reads the remote ROS publisher inventory to advertise the matching camera topic before the first encoded frame. When the camera topic disappears from that inventory, the ground publisher is withdrawn. A local reader triggers a subscription to that robot's exact Zenoh key. Matching at the airborne bridge attaches the ROS subscription to the encoder. Closing the final reader releases both subscriptions on the next discovery cycles. Previously saved ingress rules use their original always-subscribed behavior unless explicitly set to on-demand. A source route can omit `type`. The bridge selects the publisher from the peer's ROS graph using the sender's namespace preference. A custom Zenoh key transports the configured ROS topic and the ground ingress can remap its prefix. Older peers without a topic inventory can advertise explicitly typed or previously bound routes.
