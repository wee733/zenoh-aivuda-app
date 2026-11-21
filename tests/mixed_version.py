"""Run with ROS sourced: python3 tests/mixed_version.py NEW_BINARY ORIGINAL_2_1_BINARY."""
import contextlib
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
import urllib.error

from bridge_smoke import eventually, local_api, session, stage_node


def wait_for(probe, processes, timeout=15):
    def ready():
        assert all(p.poll() is None for p in processes), 'node exited'
        try:
            return probe()
        except urllib.error.URLError:
            return False
    eventually(ready, timeout)


def run(current, old, root):
    with contextlib.ExitStack() as stack:
        sockets = [stack.enter_context(socket.socket()) for _ in range(4)]
        for sock in sockets:
            sock.bind(('127.0.0.1', 0))
        manager_port, peer_port, manager_zport, peer_zport = [s.getsockname()[1] for s in sockets]
    processes = []
    manager, peer = local_api(manager_port, 8), local_api(peer_port, 8)
    peer_settings = root / 'v21' / 'settings.json'
    with contextlib.ExitStack() as files:
        try:
            for name, binary, api_port, zport, zid in [('v21', old, peer_port, peer_zport, '21'),
                                                        ('manager', current, manager_port, manager_zport, '22')]:
                directory = root / name
                directory.mkdir()
                cfg = session(zid, zport)
                if name == 'v21':
                    peer_settings.write_text(json.dumps({'default_preset': 'empty', 'allow_unprefixed_local': False,
                                                         'liveliness_enabled': False, 'domain_prefix': 'test'}))
                else:
                    binary = stage_node(binary, directory, '#!/bin/sh\n[ "$1" = status ] || exit 1\n'
                                        'printf \'%s\\n\' \'{"ok":true,"active":false}\'\n')
                    raw = json.loads(cfg['raw_json5'])
                    raw['connect'] = {'endpoints': [f'tcp/127.0.0.1:{peer_zport}']}
                    cfg['raw_json5'] = json.dumps(raw)
                runtime = directory / 'runtime.json'
                runtime.write_text(json.dumps({'agent_id': name, 'default_agent_id': name, 'domain_prefix': 'test',
                                               'session': cfg, 'api_port': api_port,
                                               'bridge_settings_file': str(directory / 'settings.json')}))
                env = dict(os.environ, ROS_HOME=str(directory), ROS_DOMAIN_ID='117', ROS_STATIC_PEERS='',
                           ROS_AUTOMATIC_DISCOVERY_RANGE='OFF', AIVUDA_APP_RUNTIME_DATA_PATH=str(directory))
                log = files.enter_context((directory / 'node.log').open('w'))
                processes.append(subprocess.Popen([binary, '--ros-args', '-p', f'runtime_config_file:={runtime}'],
                                                  env=env, stdout=log, stderr=subprocess.STDOUT))
                api = peer if name == 'v21' else manager
                wait_for(lambda: api('health')['ok'], processes)
            wait_for(lambda: any(a['agent_id'] == 'v21' for a in manager('agents')['agents']), processes)
            original = peer('health')['bridge_settings']
            assert not original['allow_unprefixed_local'] and not original['liveliness_enabled']
            manager('bridge/settings', {'agent_id': 'v21', 'bridge_settings': {'default_preset': 'vision_debug'},
                                        'save': True})
            expected = {**original, 'default_preset': 'vision_debug'}
            wait_for(lambda: peer('health')['bridge_settings'] == expected and
                     json.loads(peer_settings.read_text()) == expected, processes)
            print('PASS: 2.2 updates a real 2.1 peer; all untouched settings survive in memory and on disk')
        except BaseException:
            for path in root.glob('*/node.log'):
                print(path.read_text(), file=sys.stderr)
            raise
        finally:
            for p in reversed(processes):
                if p.poll() is None:
                    p.send_signal(signal.SIGINT)
                    try:
                        p.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        p.kill()
                        p.wait()


if __name__ == '__main__':
    with tempfile.TemporaryDirectory() as tmp:
        run(*(str(Path(p).resolve(strict=True)) for p in sys.argv[1:]), Path(tmp))
