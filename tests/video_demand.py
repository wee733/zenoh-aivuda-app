"""Exercise the H.264 preset across separate ROS domains and real loopback Zenoh peers."""
from contextlib import ExitStack
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile

import rclpy
from rclpy.context import Context
from rclpy.executors import SingleThreadedExecutor
from rclpy.qos import qos_profile_sensor_data
from sensor_msgs.msg import CompressedImage
from foxglove_msgs.msg import CompressedVideo

from bridge_smoke import eventually, local_api, session, stage_node


def free_port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


def peer(stack, directory, binary, name, domain, port, upstream=None):
    directory.mkdir()
    http = free_port()
    cfg = json.loads(session('ae' if upstream else 'af', port)['raw_json5'])
    if upstream:
        cfg['connect'] = {'endpoints': ['tcp/127.0.0.1:%d' % upstream]}
    runtime = directory / 'settings.json'
    runtime.write_text(json.dumps({'agent_id': name, 'domain_prefix': 'videotest', 'api_port': http,
                                   'default_preset': 'vision_debug', 'session': {
                                       'config_mode': 'raw_json5', 'raw_json5': json.dumps(cfg)}}))
    log = stack.enter_context((directory / 'peer.log').open('w'))
    proc = subprocess.Popen([binary, '--ros-args', '-p', 'runtime_config_file:=' + str(runtime)],
                            env=dict(os.environ, ROS_DOMAIN_ID=str(domain), ROS_AUTOMATIC_DISCOVERY_RANGE='LOCALHOST',
                                     AIVUDA_APP_RUNTIME_DATA_PATH=str(directory)), stdout=log, stderr=log)
    def cleanup():
        proc.send_signal(signal.SIGINT)
        proc.wait(timeout=8)
    stack.callback(cleanup)
    api = local_api(http, 3)
    def ready():
        try:
            return api('health')['ok']
        except OSError:
            return False
    eventually(ready)
    return api


def run(directory, binary):
    binary = stage_node(binary, directory, '#!/bin/sh\necho \'{"ok":true,"active":false}\'\n')
    with ExitStack() as stack:
        port = free_port()
        api = peer(stack, directory / 'sender', binary, 'air', 122, port)
        receiver = peer(stack, directory / 'receiver', binary, 'ground', 123, 0, port)
        nodes = []
        for domain in (122, 123):
            context = Context()
            rclpy.init(context=context, domain_id=domain)
            stack.callback(context.shutdown)
            node = rclpy.create_node('video_demand_%s' % domain, context=context)
            stack.callback(node.destroy_node)
            nodes.append(node)
        air, ground = nodes
        executor = SingleThreadedExecutor(context=ground.context)
        executor.add_node(ground)
        stack.callback(executor.shutdown)
        def round_trip(topic, message, source_prefix='', remap='air'):
            pub = air.create_publisher(message, source_prefix + '/' + topic, qos_profile_sensor_data)
            target = '/' + remap + '/' + topic
            eventually(lambda: ground.count_publishers(target) == 1)
            assert pub.get_subscription_count() == 0, 'idle ground bridge requested frames'
            rule = next(r for r in api('rules')['status']['egress'] if r['topic'] == topic)
            assert (rule['max_freq'], rule['max_kbps'], rule['on_demand'], rule['reliability'],
                    rule['congestion']) == (0, 0, True, 'best_effort', 'drop')
            received = []
            reader = ground.create_subscription(message, target, lambda frame: received.append(bytes(frame.data)),
                                                qos_profile_sensor_data)
            eventually(lambda: pub.get_subscription_count() == 1)
            frames = [bytes([0, 0, 0, 1, 0x65, i]) for i in range(15)]
            for data in frames:
                msg = message()
                msg.format = 'h264'
                msg.data = data
                pub.publish(msg)
                executor.spin_once(timeout_sec=0.2)
            def drained():
                if len(received) < len(frames):
                    executor.spin_once(timeout_sec=0.1)
                return len(received) == len(frames)
            eventually(drained, 3)
            assert received == frames, (topic, received)
            ground.destroy_subscription(reader)
            eventually(lambda: pub.get_subscription_count() == 0)
            air.destroy_publisher(pub)
            eventually(lambda: ground.count_publishers(target) == 0)
            print('PASS', target, 'idle, 15 ordered byte-identical frames, idle, publisher removal')

        round_trip('camera/video/h264', CompressedVideo)
        assert ground.count_publishers('/air/camera/image_compressed') == 0
        round_trip('camera/image_compressed', CompressedImage)

        # A custom key carries no ROS name. Infer the source type from the
        # announced ROS graph even if the dormant egress has never bound a reader.
        key = 'videotest/media/bitstream'
        receiver('rules', {'ingress': [{'pattern': 'videotest/media/**', 'on_demand': True,
                                       'remap_prefix': 'received', 'max_freq': 0, 'qos': 'best_effort'}]})
        api('rules', {'egress': [{'topic': 'camera/teststream', 'type': '', 'zenoh_key': key,
                                  'on_demand': True, 'max_freq': 0, 'reliability': 'best_effort'}]})
        # A previous protocol implementation can report a resolved type but no
        # topic inventory or effective key. Its unavailable type must not suppress
        # another valid offer matching the same ingress rule.
        legacy = {'agent_id': 'a_legacy', 'domain_prefix': 'videotest', 'egress': [
            {'topic': 'camera/unsupported', 'zenoh_key': 'videotest/media/broken',
             'type': 'missing_video_pkg/msg/Video'},
            {'topic': 'camera/legacy', 'zenoh_key': 'videotest/media/legacy',
             'resolved_type': 'sensor_msgs/msg/CompressedImage'},
        ]}
        api('zenoh/put', {'key_expr': 'videotest/meta/a_legacy/status', 'payload': json.dumps(legacy)})
        eventually(lambda: ground.count_publishers('/received/camera/legacy') == 1)
        assert ground.count_publishers('/received/camera/teststream') == 0
        round_trip('camera/teststream', CompressedImage, '/air', 'received')
        assert ground.count_publishers('/received/bitstream') == 0


if __name__ == '__main__':
    with tempfile.TemporaryDirectory() as tmp:
        run(Path(tmp), sys.argv[1])
