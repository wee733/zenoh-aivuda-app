"""Loopback integration test for heartbeat, connected neighbors and EventSource clients."""
import argparse
from contextlib import ExitStack
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
from tempfile import TemporaryDirectory
import time
import urllib.error
import urllib.parse
import urllib.request

from bridge_smoke import eventually, local_api, session, stage_node


def events(response):
    for line in response:
        if line.startswith(b'data: '):
            yield json.loads(line[6:])


def run(binary, directory, publish_heartbeat):
    with socket.socket() as http, socket.socket() as zenoh:
        for sock in (http, zenoh):
            sock.bind(('127.0.0.1', 0))
        port, zenoh_port = http.getsockname()[1], zenoh.getsockname()[1]
    config = directory / 'runtime.json'
    config.write_text(json.dumps({
        'agent_id': 'observer', 'robot_type': 'ground_station', 'api_port': port,
        'session': session('d5', zenoh_port), 'heartbeat_enabled': publish_heartbeat,
    }))
    env = dict(os.environ, AIVUDA_OS_AVAHI_HOSTNAME='observer-mdns', AIVUDA_APP_RUNTIME_DATA_PATH=str(directory), ROS_HOME=str(directory),
               ROS_AUTOMATIC_DISCOVERY_RANGE='OFF', ROS_DOMAIN_ID='169', ROS_STATIC_PEERS='')
    api = local_api(port, timeout=3)
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    stream_path = f'http://127.0.0.1:{port}/api/zenoh/stream'
    def connect(selectors):
        return opener.open(stream_path + '?' + urllib.parse.urlencode({'key_expr': selectors}), timeout=3)
    def publish(key, obj):
        api('zenoh/put', {'key_expr': key, 'payload': json.dumps(obj)})
    with (directory / 'node.log').open('w+') as log, ExitStack() as clients:
        proc = subprocess.Popen([binary, '--ros-args', '-p', f'runtime_config_file:={config}'],
                                env=env, stdout=log, stderr=log)
        try:
            def ready():
                if proc.poll() is not None:
                    raise RuntimeError(f'node exited with {proc.returncode}')
                try:
                    return api('health')['ok']
                except urllib.error.URLError:
                    return False
            eventually(ready)
            for expr in ('', '**', '*/ctrl/**', '*/heartbeat,', '*/telemetry/*,uav1/telemetry/state'):
                api('zenoh/stream?' + urllib.parse.urlencode({'key_expr': expr}), expected=400)
            selectors = 'observer/heartbeat,observer/comm_status,uav1/telemetry/*'
            stream = clients.enter_context(connect(selectors))
            assert stream.headers['Content-Type'] == 'text/event-stream'
            assert not stream.headers.get('Content-Length') and not stream.headers.get('Access-Control-Allow-Origin')
            sample = {'data': {'x': 2}}
            publish('other/telemetry/state', {'unexpected': True})
            publish('uav1/telemetry/local_pos', sample)
            publish('uav1/heartbeat', {'avahi_hostname': 'aircraft', 'robot_type': 'uxrce_rotor', 'online': True})
            local_heartbeat = None
            local_pos = None
            observed = False
            expired = False
            deadline = time.monotonic() + 8
            for event in events(stream):
                assert time.monotonic() < deadline, (local_heartbeat, local_pos, observed, expired)
                key, obj = event['key_expr'], event['payload']
                if key == 'observer/heartbeat':
                    assert publish_heartbeat, obj
                    assert obj['online'] and obj['robot_type'] == 'ground_station' and obj['agent_id'] == 'observer'
                    assert obj['avahi_hostname'] == 'observer-mdns' and obj['timestamp_unix_ms'] > 0
                    local_heartbeat = obj
                elif key == 'uav1/telemetry/local_pos':
                    assert obj == sample
                    local_pos = obj
                else:
                    assert key == 'observer/comm_status' and obj['self']['key_prefix'] == 'observer'
                    neighbors = obj['connected_neighbors']
                    if neighbors:
                        peer, = neighbors
                        assert peer['key_prefix'] == 'uav1' and peer['avahi_hostname'] == 'aircraft'
                        assert peer['robot_type'] == 'uxrce_rotor' and peer['last_seen_unix_ms'] > 0
                        observed = True
                    elif observed:
                        expired = True
                if expired and local_pos and (local_heartbeat or not publish_heartbeat):
                    break
            assert expired and local_pos and (local_heartbeat or not publish_heartbeat)
            if publish_heartbeat:
                others = [clients.enter_context(connect('*/heartbeat')) for _ in range(15)]
                api('zenoh/stream?key_expr=*/heartbeat', expected=503)
                others.pop().close()
                def recovered():
                    try:
                        clients.enter_context(connect('*/heartbeat'))
                        return True
                    except urllib.error.HTTPError as exc:
                        assert exc.code == 503
                        return False
                eventually(recovered, 4)
                api('zenoh/session', {'session': session('d6', zenoh_port), 'save': False})
                assert len(stream.read()) < 100_000  # Session change closes the old subscriptions.
                with connect('observer/heartbeat') as resumed:
                    assert next(events(resumed))['payload']['agent_id'] == 'observer'
            proc.send_signal(signal.SIGINT)
            assert proc.wait(timeout=3) == 0
            print(f'PASS: presence, peer expiry, selective SSE and shutdown (heartbeat={publish_heartbeat})')
        except BaseException:
            log.seek(0)
            print(log.read())
            raise
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('binary')
    args = parser.parse_args()
    with TemporaryDirectory() as tmp:
        root = Path(tmp)
        executable = stage_node(Path(args.binary).resolve(), root,
                                '#!/bin/sh\n[ "$1" = status ] && echo \'{"ok":true,"active":false}\'\n')
        for enabled in (True, False):
            directory = root / str(enabled)
            directory.mkdir()
            run(executable, directory, enabled)
