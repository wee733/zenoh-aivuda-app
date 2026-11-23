"""Fake-helper transaction tests: source ROS, then python3 tests/shield_api.py BUILD/zenoh_bridge_node."""

import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error

from bridge_smoke import eventually, local_api, session, stage_node


HELPER = '''#!/bin/bash
exec /usr/bin/python3 -I - "$@" <<'PY'
import json, os, pathlib, subprocess, sys
root = pathlib.Path(os.environ['AIVUDA_APP_RUNTIME_DATA_PATH'])
action, scope, domains, *owner = sys.argv[1:]
assert owner in ([str(root / 'settings.json')], ['--owner', str(root / 'settings.json')]), owner
with (root / 'calls').open('a') as f:
    print(json.dumps([action, scope, domains]), file=f)
state = root / 'active'
fail = root / 'fail'
if fail.exists() and fail.read_text() == action:
    print(json.dumps({'ok': True, 'active': action == 'enable'}))
    sys.exit(9)  # A plausible JSON body cannot turn a failed process into success.
if action == 'enable': state.write_text(json.dumps([scope, domains]))
elif action == 'disable': state.unlink(missing_ok=True)
else: assert action in ('status', 'snapshot')
if action == 'status' and (root / 'stall').exists():
    # Parent exits but the descendant holds stdout; killing only the shell
    # or timing out waitpid without also bounding the read would hang.
    child = subprocess.Popen(['sleep', '45'])
    (root / 'stalled').write_text(str(child.pid))
    sys.exit(0)
actual = json.loads(state.read_text()) if state.exists() else [scope, '']
print(json.dumps({'ok': True, 'active': state.exists(), 'scope': actual[0], 'effective_domains': actual[1]}))
PY
'''


def check(binary, root):
    binary = stage_node(binary, root, HELPER)
    with socket.socket() as http, socket.socket() as zenoh:
        http.bind(('127.0.0.1', 0))
        zenoh.bind(('127.0.0.1', 0))
        port, zport = http.getsockname()[1], zenoh.getsockname()[1]
    settings = root / 'settings.json'
    runtime = root / 'runtime.json'
    runtime.write_text(json.dumps({'agent_id': 'shield-test', 'domain_prefix': 'test', 'api_port': port,
                                   'bridge_settings_file': str(settings), 'session': session('b1', zport)}))
    env = dict(os.environ, AIVUDA_APP_RUNTIME_DATA_PATH=str(root), ROS_DOMAIN_ID='117',
               ROS_HOME=str(root), ROS_AUTOMATIC_DISCOVERY_RANGE='OFF', ROS_STATIC_PEERS='',
               ZENOH_BRIDGE_SHIELD_TOKEN='test-only')
    api = local_api(port, timeout=10, token=env['ZENOH_BRIDGE_SHIELD_TOKEN'])

    def rpc(action, **fields):
        data = api('zenoh/get', {'key_expr': 'test/ctrl/shield-test/rules', 'timeout_ms': 2000,
                                 'payload': json.dumps({'action': action, **fields})})
        return json.loads(data['replies'][0]['payload'])

    def mutations():
        return [c for c in map(json.loads, (root / 'calls').read_text().splitlines()) if c[0] in ('enable', 'disable')]

    def policy():
        return api('health')['bridge_settings']

    # Model a restart after a transient enable: disk says disabled but an
    # independently-selected scope and domain list remain installed.
    retained = ['all', '11,112']
    (root / 'active').write_text(json.dumps(retained))
    with (root / 'node.log').open('w+') as log:
        proc = subprocess.Popen([binary, '--ros-args', '-p', f'runtime_config_file:={runtime}'],
                                env=env, stdout=log, stderr=subprocess.STDOUT)
        try:
            def ready():
                assert proc.poll() is None, 'node exited'
                try:
                    return api('health')['ok']
                except urllib.error.URLError:
                    return False
            eventually(ready, 12)

            assert api('status')['dds_shield_token_required']
            api('dds_shield', {'enabled': True}, extra_headers={'X-DDS-Shield-Token': 'incorrect'}, expected=403)
            # Empty configured domains must remain distinguishable from the running domain.
            local = next(a for a in api('agents')['agents'] if a['is_local'])
            assert (local['dds_shield_configured_domains'], local['dds_shield_domains']) == ('', '117')
            # No write/helper call when another admin changed policy after the last browser poll.
            api('dds_shield', {'enabled': True, 'expected_policy': {'enabled': False, 'scope': 'all',
                                                                    'domains': ''}}, admin=True, expected=409)
            assert not mutations() and not settings.exists()

            # Both write routes reject conflicting selectors before resolving an empty one as local.
            for route in ('dds_shield', 'bridge/settings'):
                api(route + '?agent_id=another', {'agent_id': ''}, admin=True, expected=400)

            # Rejected on both local settings routes, regardless of save or target selector.
            api('dds_shield', {'enabled': True}, expected=403)
            api('dds_shield?agent_id=someone-else', {'enabled': True}, admin=True, expected=403)
            api('bridge/settings', {'bridge_settings': {'dds_shield_enabled': True}}, expected=403)
            assert not rpc('set_dds_shield', enabled=True)['ok']
            assert not rpc('set_bridge_settings', bridge_settings={'dds_shield_scope': 'all'})['ok']
            assert not settings.exists() and not mutations()

            # Invalid even with enabled=false, before calling the privileged helper or writing.
            for bad in ('1,', ',0', '1,,2', '1;2', '-1', '1.0', '233', 'a', '0000'):
                api('dds_shield', {'enabled': False, 'domains': bad}, admin=True, expected=400)
            api('bridge/settings', {'bridge_settings': {'dds_shield_scope': 'typo'}}, admin=True, expected=400)
            assert not settings.exists() and not mutations()

            # An unrelated save must not rewrite an ambiguous on-disk shield policy.
            ambiguous = '{"dds_shield_enabled":true,"dds_shield_enabled":false}'
            settings.write_text(ambiguous)
            api('bridge/settings', {'bridge_settings': {'agent_id': 'wrong'}}, expected=500)
            assert settings.read_text() == ambiguous and api('health')['local_agent_id'] == 'shield-test'
            settings.unlink()

            # Editing parameters (on either route) stages them without disabling
            # the retained table. Failed snapshots cannot start a mutation.
            api('dds_shield', {'domains': '3', 'save': False}, admin=True)
            api('bridge/settings', {'bridge_settings': {'dds_shield_domains': '4'}, 'save': False}, admin=True)
            assert not mutations() and json.loads((root / 'active').read_text()) == retained
            (root / 'fail').write_text('snapshot')
            api('dds_shield', {'enabled': True}, admin=True, expected=503)
            (root / 'fail').unlink()
            assert not mutations() and not settings.exists()
            # Fail the save after changing the firewall. Restore kernel policy,
            # not configured disabled/domain:4 or the new requested parameters.
            settings.mkdir()
            api('dds_shield', {'enabled': True}, admin=True, expected=500)
            assert mutations() == [['enable', 'domain', '4'], ['enable', *retained]]
            assert json.loads((root / 'active').read_text()) == retained
            assert api('dds_shield', expected=503)['active'] and not policy()['dds_shield_enabled']
            settings.rmdir()
            api('dds_shield', {'enabled': False, 'save': False}, admin=True)
            assert not (root / 'active').exists()
            (root / 'calls').write_text('')

            # Dedicated and generic API share one operation; normalize before helper + disk.
            api('dds_shield', {'enabled': True, 'domains': ' 12,002,12 '}, admin=True)
            assert mutations() == [['enable', 'domain', '2,12']]
            assert json.loads(settings.read_text())['dds_shield_domains'] == '2,12'
            api('bridge/settings', {'bridge_settings': {'dds_shield_scope': 'all'}}, admin=True)
            assert mutations()[-1] == ['enable', 'all', '2,12'] and len(mutations()) == 2
            saved = settings.read_bytes()

            # A failing combined update cannot rename, save, or claim cached success.
            (root / 'fail').write_text('disable')
            api('bridge/settings', {'bridge_settings': {'agent_id': 'wrong', 'dds_shield_enabled': False}},
                admin=True, expected=503)
            assert settings.read_bytes() == saved and api('health')['local_agent_id'] == 'shield-test'
            assert policy()['dds_shield_enabled'] and not rpc('get_dds_shield')['ok']
            (root / 'fail').write_text('status')
            api('dds_shield', expected=503)
            (root / 'fail').unlink()
            # No privileged getter on RPC. Timer must recover a false cache while desired enabled.
            eventually(lambda: rpc('get_dds_shield')['ok'], 8)

            # While the poll helper is stuck, the default ROS group (shared by
            # status publication and egress subscriptions) must continue to run.
            api('zenoh/sniff/start', {'key_expr': 'test/meta/shield-test/status'})
            (root / 'stall').touch()
            eventually(lambda: (root / 'stalled').exists(), 8)
            start_ms = time.time_ns() // 1_000_000
            eventually(lambda: sum(s['timestamp_ms'] > start_ms for s in api('zenoh/sniff')['samples']) >= 2, 3.5)
            (root / 'stall').unlink()
            eventually(lambda: 'timed out' in rpc('get_dds_shield')['error'], 5)
            child_stat = Path('/proc') / (root / 'stalled').read_text() / 'stat'
            eventually(lambda: not child_stat.exists() or child_stat.read_text().split()[2] == 'Z', 4)
            api('zenoh/sniff/stop', {})
            assert api('dds_shield')['active']

            # Force a persistence failure *after* a successful helper and reconnect.
            settings.unlink()
            settings.mkdir()
            count = len(mutations())
            api('bridge/settings', {'bridge_settings': {'agent_id': 'wrong', 'dds_shield_enabled': False}},
                admin=True, expected=500)
            assert mutations()[count:] == [['disable', 'all', '2,12'], ['enable', 'all', '2,12']]
            assert api('health')['local_agent_id'] == 'shield-test' and api('dds_shield')['active']
            settings.rmdir()
            settings.write_bytes(saved)

            # A deferred RPC merges at execution; no stale reconstruction of the shield.
            # Accept a full unchanged policy as well as a genuinely partial patch.
            assert rpc('set_bridge_settings', bridge_settings=policy(), save=False)['queued']
            assert rpc('patch_bridge_settings', bridge_settings={'default_preset': 'vision_debug'})['queued']
            count = len(mutations())
            api('dds_shield', {'enabled': False, 'save': False}, admin=True)
            eventually(lambda: policy()['default_preset'] == 'vision_debug', 4)
            assert not policy()['dds_shield_enabled'] and mutations()[count:] == [['disable', 'all', '2,12']]
            assert json.loads(settings.read_text())['dds_shield_enabled']  # remote save did not persist transient policy
            api('bridge/settings', {'bridge_settings': {'dds_shield_enabled': False, 'allow_unprefixed_local': False}})
            assert json.loads(settings.read_text())['dds_shield_enabled']

            # A wildcard target resolves over Zenoh, not the local shortcut. The proxy
            # strips echoed fields and will not forward a changed policy even with a token.
            api('bridge/settings', {'agent_id': '*', 'bridge_settings': {'dds_shield_enabled': True}},
                admin=True, expected=403)
            api('bridge/settings', {'agent_id': '*', 'bridge_settings': {'dds_shield_enabled': False,
                                                                                 'default_preset': 'formation'}})
            eventually(lambda: policy()['default_preset'] == 'formation', 4)
            assert mutations()[count:] == [['disable', 'all', '2,12']]
        except BaseException:
            log.seek(0)
            print(log.read(), file=sys.stderr)
            raise
        finally:
            if proc.poll() is None:
                proc.send_signal(signal.SIGINT)
                try:
                    proc.wait(timeout=8)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()
                    raise


def token_free_check(binary, root, bind_host):
    executable = stage_node(binary, root, HELPER)
    with socket.socket() as http, socket.socket() as zenoh:
        http.bind(('127.0.0.1', 0))
        zenoh.bind(('127.0.0.1', 0))
        port, zport = http.getsockname()[1], zenoh.getsockname()[1]
    settings = root / 'settings.json'
    runtime = root / 'runtime.json'
    runtime.write_text(json.dumps({'agent_id': 'shield-test', 'api_bind_host': bind_host, 'api_port': port,
                                   'bridge_settings_file': str(settings), 'session': session('b2', zport)}))
    env = dict(os.environ, AIVUDA_APP_RUNTIME_DATA_PATH=str(root), ROS_DOMAIN_ID='117', ROS_HOME=str(root),
               ROS_AUTOMATIC_DISCOVERY_RANGE='OFF', ROS_STATIC_PEERS='', AIVUDA_OS_AVAHI_HOSTNAME='fixture-uav')
    env.pop('ZENOH_BRIDGE_SHIELD_TOKEN', None)
    api = local_api(port, timeout=5)
    with (root / 'node.log').open('w') as log:
        proc = subprocess.Popen([executable, '--ros-args', '-p', f'runtime_config_file:={runtime}'],
                                env=env, stdout=log, stderr=subprocess.STDOUT)
        try:
            def ready():
                assert proc.poll() is None, 'node exited'
                try:
                    return api('status')['ok']
                except urllib.error.URLError:
                    return False
            eventually(ready, 12)
            if bind_host != '127.0.0.1':
                assert api('status')['dds_shield_token_required']
                assert not api('status')['dds_shield_management_available']
                api('dds_shield', {'enabled': True}, expected=403)
                return
            assert not api('status')['dds_shield_token_required']
            local = next(a for a in api('agents')['agents'] if a['is_local'])
            assert not local['dds_shield_token_required'] and local['dds_shield_management_available']
            for headers in ({'Origin': 'https://elsewhere.invalid'}, {'Sec-Fetch-Site': 'cross-site'},
                            {'Host': 'unrelated.invalid'}, {'X-Forwarded-For': '192.0.2.1'}):
                api('dds_shield', {'enabled': True}, extra_headers=headers, expected=403)
                api('bridge/settings', {'bridge_settings': {'dds_shield_enabled': True}},
                    extra_headers=headers, expected=403)
            api('dds_shield?agent_id=remote', {'enabled': True}, expected=403)
            assert not settings.exists()
            def proxy_headers(host, client, scheme='https'):
                return {'Host': host, 'Origin': scheme + '://' + host, 'Sec-Fetch-Site': 'same-origin',
                        'X-Forwarded-For': client, 'X-Forwarded-Host': host, 'X-Forwarded-Proto': scheme}

            gateway = proxy_headers('localhost:8080', '127.0.0.1', 'http')
            api('dds_shield', {'enabled': True}, extra_headers=gateway)
            assert json.loads(settings.read_text())['dds_shield_enabled']
            api('dds_shield', {'enabled': False})
            assert not json.loads(settings.read_text())['dds_shield_enabled']

            # Read numeric NIC addresses from the kernel. No external network or DNS is needed.
            interfaces = json.loads(subprocess.check_output(['ip', '-j', 'addr', 'show', 'up']))
            addresses = [addr['local'] for iface in interfaces for addr in iface.get('addr_info', [])
                         if addr['scope'] == 'global' and addr['family'] in ('inet', 'inet6')]
            alias = 'fixture-uav.local'
            for client in ['::1', *addresses]:
                via_alias = proxy_headers(alias, client)
                assert api('status', extra_headers=via_alias)['dds_shield_management_available']
                api('dds_shield', {'enabled': False}, extra_headers=via_alias)
                host_ip = '[' + client + ']' if ':' in client else client
                assert api('status', extra_headers=proxy_headers(host_ip, client))['dds_shield_management_available']
                for change in ({'X-Forwarded-For': '192.0.2.44'}, {'X-Forwarded-For': client + ', ' + client},
                               {'Origin': 'http://' + alias}, {'Sec-Fetch-Site': 'cross-site'},
                               {'X-Forwarded-Host': 'other.invalid'}, {'X-Forwarded-Proto': 'unknown'}):
                    api('dds_shield', {'enabled': True}, expected=403, extra_headers={**via_alias, **change})
            api('dds_shield', {'enabled': True}, expected=403, extra_headers={'Host': alias})
            assert not json.loads(settings.read_text())['dds_shield_enabled']
            for route in ('config/export', 'config/import'):
                api(route, {} if route.endswith('import') else None,
                    extra_headers={'Origin': 'https://elsewhere.invalid'}, expected=403)
        finally:
            proc.send_signal(signal.SIGINT)
            proc.wait(timeout=8)


if __name__ == '__main__':
    with tempfile.TemporaryDirectory() as tmp:
        check(str(Path(sys.argv[1]).resolve(strict=True)), Path(tmp))
    for bind_host in ('127.0.0.1', '0.0.0.0'):
        with tempfile.TemporaryDirectory() as tmp:
            token_free_check(str(Path(sys.argv[1]).resolve(strict=True)), Path(tmp), bind_host)
    print('PASS: local authorization, remote patches, validation, single apply, helper errors, rollback and status')
