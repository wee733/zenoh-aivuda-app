"""Run after sourcing ROS: python3 tests/dds_shield_startup.py build/zenoh_bridge_node."""
import fcntl
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import time

from bridge_smoke import stage_node


def run(source, root):
    binary = stage_node(source, root, '''set -eu
printf '%s\\n' "$@" >> "$CALLS"
printf '%s\\n' "$MOCK_RESPONSE"
exit "$MOCK_RC"
''')
    scripts = root / 'scripts'
    helper = scripts / 'dds_shield.sh'
    data = root / 'data'
    data.mkdir()
    calls = root / 'calls'
    settings = data / 'bridge_settings.json'
    runtime = data / 'runtime_config.json'
    # Record the exact init boundary; no RMW or packet capture needed.
    shim = root / 'observe.so'
    subprocess.run(['cc', '-shared', '-fPIC', '-x', 'c', '-', '-o', shim], check=True,
                   input='''#include <stdlib.h>
#include <unistd.h>
void rcl_init(void) { _exit(73); }
''', text=True)
    env = dict(os.environ, AIVUDA_APP_RUNTIME_DATA_PATH=str(data), CALLS=str(calls),
               MOCK_RC='0', ROS_HOME=str(root), ROS_DOMAIN_ID='7', ROS_STATIC_PEERS='',
               ROS_AUTOMATIC_DISCOVERY_RANGE='OFF', LD_PRELOAD=str(shim),
               MOCK_RESPONSE=json.dumps({'ok': True, 'active': True}))

    def check(*args, rc=1, expected=(), executable=binary, **changes):
        calls.write_text('')
        p = subprocess.run([executable, *args], env={**env, **changes}, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, text=True, timeout=8)
        assert p.returncode == rc, (args, p.returncode, p.stdout)
        assert calls.read_text().splitlines() == list(expected), (args, calls.read_text(), p.stdout)

    enabled = {'dds_shield_enabled': True}
    settings.write_text(json.dumps(enabled))
    expected = ['enable', 'domain', '7', str(settings)]
    check(rc=73, expected=expected)
    for response, helper_rc in [('{}', '0'), ('not JSON', '0'), ('{"ok":true,"active":false}', '0'),
                                ('{"ok":true,"active":true,"rx_dropped_packets":-1}', '0'),
                                ('{"ok":true,"active":true}', '1')]:
        check(expected=expected, MOCK_RESPONSE=response, MOCK_RC=helper_rc)
    helper.rename(scripts / 'hidden')
    check()
    (scripts / 'hidden').rename(helper)
    for value in ['{', '[]', 'null', '{} trailing', '{"dds_shield_enabled":"true"}',
                  '{"dds_shield_domains":"1,bad"}', '{"dds_shield_scope":"unknown"}',
                  '{"dds_shield_enabled":true,"dds_shield_enabled":false}']:
        settings.write_text(value)
        check()
    settings.unlink()
    settings.symlink_to(data / 'missing')
    check()
    settings.unlink()
    settings.mkdir()
    check()
    settings.rmdir()
    settings.write_text(json.dumps(enabled))
    if os.getuid() != 0:
        settings.chmod(0)
        try:
            check()
        finally:
            settings.chmod(0o600)
    with Path(str(settings) + '.lock').open('r') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        check()
    print('PASS: direct launch enforces before rcl_init; failed helper, invalid files and owner conflict stop startup')

    other = data / 'another.json'
    other.write_text(json.dumps({**enabled, 'dds_shield_domains': '19,2'}))
    rt = data / 'alternative-runtime.json'
    rt.write_text(json.dumps({'bridge_settings_file': str(other), 'dds_shield_enabled': False}))
    scoped = root / 'scoped.yaml'
    scoped.write_text(f'/fleet/renamed:\n  ros__parameters:\n    runtime_config_file: "{rt}"\n')
    check('--ros-args', '-r', '__node:=renamed', '-r', '__ns:=/fleet', '--params-file', str(scoped),
          rc=73, expected=['enable', 'domain', '2,19', str(other)])
    check('--ros-args', '-p', f'bridge_settings_file:={other}', rc=73,
          expected=['enable', 'domain', '2,19', str(other)])
    check('--ros-args', '-p', f'bridge_settings_file:={settings}', '-p', f'runtime_config_file:={rt}')
    other.write_text('{}')
    check('--ros-args', '-p', f'bridge_settings_file:={other}', '-p', 'dds_shield_enabled:=true',
          '-p', 'ros_domain_id:=17', rc=73, expected=['enable', 'domain', '17', str(other)])
    rt.write_text(json.dumps({'bridge_settings_file': str(other), 'dds_shield_script': '/bin/true'}))
    check('--ros-args', '-p', f'runtime_config_file:={rt}')
    print('PASS: scoped ROS parameter files, direct overrides, strict trusted-helper path')

    # Exercise the packaged launcher with the same mock, not just the binary.
    repo = Path(__file__).resolve().parents[1]
    shutil.copy2(repo / 'start.sh', root / 'start.sh')
    shutil.copy2(repo / 'scripts/ros_setup.sh', scripts / 'ros_setup.sh')
    cfg = root / 'default.yaml'
    cfg.write_text('ros:\n  domain_id: 7\n')
    check(executable=root / 'start.sh', rc=73, expected=expected, AIVUDA_APP_CONFIG_PATH=str(cfg),
          HOME=str(root))
    print('PASS: packaged launcher uses the same enforced startup path')

    # Real ROS shutdown in an isolated domain with multicast scouting disabled;
    # the only shell helper present is the private recorder above.
    runtime.write_text(json.dumps({'api_port': 0, 'session': {'config_mode': 'raw_json5',
        'raw_json5': json.dumps({'mode': 'peer', 'listen': {'endpoints': ['tcp/127.0.0.1:0']},
            'scouting': {'multicast': {'enabled': False}, 'gossip': {'enabled': False}}})}}))
    calls.write_text('')
    with (root / 'node.log').open('w+') as log:
        p = subprocess.Popen([binary], env={k: v for k, v in env.items() if k != 'LD_PRELOAD'},
                             stdout=log, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + 12
            while True:
                output = (root / 'node.log').read_text()
                if 'Zenoh Bridge v2.2 started' in output:
                    break
                assert p.poll() is None and time.monotonic() < deadline, output
                time.sleep(0.05)
            blocked = subprocess.run([binary], env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                     timeout=5, text=True)
            assert blocked.returncode == 1 and 'exclusively own' in blocked.stdout, blocked.stdout
            p.send_signal(signal.SIGINT)
            assert p.wait(timeout=8) == 0
        finally:
            if p.poll() is None:
                p.kill()
                p.wait()
    assert calls.read_text().splitlines() == expected, calls.read_text()
    check(rc=73, expected=expected)  # Restart re-verifies rather than trusting cached state.
    print('PASS: graceful shutdown retains owner policy; restart verifies it again')


if __name__ == '__main__':
    with tempfile.TemporaryDirectory() as tmp:
        run(Path(sys.argv[1]).resolve(strict=True), Path(tmp))
