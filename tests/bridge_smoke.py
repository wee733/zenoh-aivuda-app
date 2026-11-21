"""Source ROS 2, then python3 tests/bridge_smoke.py BUILD/zenoh_bridge_node [--concurrency].

--concurrency also tests real topic bindings and shutdown, using rclpy, std_msgs
and a second instance of the same binary. Both DDS and Zenoh stay on localhost.
"""

import concurrent.futures
import json
import os
from pathlib import Path
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request


def session(zid, listen_port):
    config = {
        "id": zid, "mode": "peer",
        "listen": {"endpoints": [f"tcp/127.0.0.1:{listen_port}"]},
        "scouting": {"multicast": {"enabled": False}, "gossip": {"enabled": False}},
    }
    return {"config_mode": "raw_json5", "raw_json5": json.dumps(config)}


def stage_node(binary, directory, helper):
    """Copy the binary next to a fake helper; never use the host firewall in tests."""
    executable = directory / "build" / "zenoh_bridge_node"
    executable.parent.mkdir()
    shutil.copy2(binary, executable)
    script = directory / "scripts" / "dds_shield.sh"
    script.parent.mkdir()
    script.write_text(helper)
    script.chmod(0o700)
    return str(executable.resolve())


def local_api(port, timeout, token=None):
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))

    def api(path, body=None, *, admin=False, expected=200):
        headers = {'Content-Type': 'application/json'}
        if admin:
            assert token is not None
            headers['X-DDS-Shield-Token'] = token
        req = urllib.request.Request(f'http://127.0.0.1:{port}/api/{path}', headers=headers,
                                     data=None if body is None else json.dumps(body).encode())
        try:
            resp = opener.open(req, timeout=timeout)
        except urllib.error.HTTPError as exc:
            resp = exc
        with resp:
            result = json.load(resp)
            assert resp.status == expected and result['ok'] == (expected == 200), (path, resp.status, result)
            return result

    return api


def eventually(check, timeout=8):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if check():
            return
        time.sleep(.03)
    raise AssertionError(f"timed out waiting for {check.__name__}")


def check_rename_bindings(api, env):
    import threading
    from unittest.mock import patch
    import rclpy
    from std_msgs.msg import String

    with patch.dict(os.environ, env):
        rclpy.init(args=[])
        node = rclpy.create_node('binding_test')
        stop = threading.Event()
        def scan():
            while not stop.is_set():
                api('rules')
        try:
            pubs = {t: node.create_publisher(String, t, 10) for t in ('/regression/pref', '/renamed/pref', '/bare')}
            api('rules', {'egress': [{'topic': 'pref'}, {'topic': 'bare'}]})
            with concurrent.futures.ThreadPoolExecutor(3) as pool:
                scans = [pool.submit(scan) for _ in range(3)]
                try:
                    for agent in ('renamed', 'regression') * 4:
                        api('bridge/settings', {'bridge_settings': {'agent_id': agent}, 'save': False})
                        eventually(lambda: {e['bound_topic'] for e in api('rules')['status']['egress']} ==
                                   {f'/{agent}/pref', '/bare'})
                        api('zenoh/sniff/start', {'key_expr': 'test/data/**'})
                        def forwarded():
                            for topic, pub in pubs.items():
                                pub.publish(String(data=topic))
                            samples = api('zenoh/sniff')['samples']
                            for s in samples:
                                assert s['key_expr'].split('/')[2] == json.loads(s['attachment'])['s'] == agent, s
                            return {s['key_expr'] for s in samples} == {f'test/data/{agent}/{t}' for t in ('pref', 'bare')}
                        eventually(forwarded)
                        api('zenoh/sniff/stop', {})
                finally:
                    stop.set()
                for scan_job in scans:
                    scan_job.result()
        finally:
            node.destroy_node()
            rclpy.shutdown()
    print('PASS: 8 renames under concurrent discovery preserve prefixed/bare bindings, Zenoh keys and source IDs')


def check_pending_shutdown(api, proc, binary, directory, listen_port, env, log):
    peer_dir = directory / 'pending-peer'
    peer_dir.mkdir()
    cfg = json.loads(session('d3', 0)['raw_json5'])
    cfg['connect'] = {'endpoints': [f'tcp/127.0.0.1:{listen_port}']}
    path = peer_dir / 'runtime.json'
    path.write_text(json.dumps({'agent_id': 'pending', 'domain_prefix': 'test', 'api_port': 0,
                                'session': {'config_mode': 'raw_json5', 'raw_json5': json.dumps(cfg)}}))
    peer = subprocess.Popen([binary, '--ros-args', '-p', f'runtime_config_file:={path}'], stdout=log, stderr=log,
                            env={**env, 'AIVUDA_APP_RUNTIME_DATA_PATH': str(peer_dir)})
    try:
        key = 'test/ctrl/pending/rules'
        eventually(lambda: api('zenoh/get', {'key_expr': key, 'timeout_ms': 500})['count'] > 0)
        api('logs?agent_id=pending')  # Both operations successfully reach the peer before it stops replying.
        peer.send_signal(signal.SIGSTOP)
        os.waitpid(peer.pid, os.WUNTRACED)
        with concurrent.futures.ThreadPoolExecutor(2) as pool:
            get = pool.submit(api, 'zenoh/get', {'key_expr': key, 'timeout_ms': 10000})
            rpc = pool.submit(api, 'logs?agent_id=pending')
            done, _ = concurrent.futures.wait((get, rpc), timeout=.25)
            assert not done, 'expected pending HTTP queries'
            proc.send_signal(signal.SIGINT)
            assert proc.wait(timeout=2) == 0, 'shutdown waited for query timeout'
    finally:
        peer.kill()
        peer.wait()
    print('PASS: SIGINT drains pending GET and remote RPC before their timeouts')


def run(binary, directory, concurrency=False):
    binary = stage_node(binary, directory, '''#!/bin/sh
[ "$1" = "status" ] || exit 1
printf '%s\\n' '{"ok":true,"active":false}'
''')
    with socket.socket() as http, socket.socket() as zenoh:
        http.bind(("127.0.0.1", 0))
        zenoh.bind(("127.0.0.1", 0))
        port, listen_port = http.getsockname()[1], zenoh.getsockname()[1]
    api = local_api(port, timeout=8)

    config_path = directory / "runtime.json"
    config_path.write_text(json.dumps({
        "agent_id": "regression", "domain_prefix": "test", "api_port": port,
        "session": session("a1", listen_port), "default_preset": "empty",
    }))
    env = dict(os.environ, ROS_HOME=str(directory), ROS_AUTOMATIC_DISCOVERY_RANGE="LOCALHOST" if concurrency else "OFF",
               ROS_DOMAIN_ID="117", ROS_STATIC_PEERS="", AIVUDA_APP_RUNTIME_DATA_PATH=str(directory))
    if concurrency:
        env['ROS_LOCALHOST_ONLY'] = '1'  # Older ROS distributions do not know ROS_AUTOMATIC_DISCOVERY_RANGE.
    with (directory / "node.log").open("w+") as log:
        proc = subprocess.Popen([binary, "--ros-args", "-p", f"runtime_config_file:={config_path}"],
                                env=env, stdout=log, stderr=subprocess.STDOUT)
        try:
            def ready():
                if proc.poll() is not None:
                    raise RuntimeError(f"node exited: {proc.returncode}")
                try:
                    return api('health')['ok']
                except urllib.error.URLError:
                    return False
            eventually(ready, 12)

            key = "test/ctrl/regression/rules"
            result = api("zenoh/get", {"key_expr": key, "timeout_ms": 1500})
            assert any(r["ok"] and json.loads(r["payload"])["ok"] for r in result["replies"]), result
            print("PASS: Lab GET received local control reply")

            for path in ("zenoh/get", "zenoh/liveliness/get"):
                for timeout in (-1, 10001, 2**64 - 1, 1.5, "3000", True, None):
                    api(path, {"key_expr": key, "timeout_ms": timeout}, expected=400)
                for timeout in (0, 1, 10000):
                    api(path, {"key_expr": key, "timeout_ms": timeout})
            print("PASS: both GET endpoints validate timeout type and bounds")

            # Local liveliness replies can arrive inline before get() returns.
            # A blocking 128-slot FIFO previously hung on the 129th reply.
            for i in range(160):
                api("zenoh/liveliness/declare", {"key_expr": f"test/burst/{i}"})
            result = api("zenoh/liveliness/get", {"key_expr": "test/burst/**"})
            assert set(result["tokens"]) == {f"test/burst/{i}" for i in range(160)}, result
            print("PASS: local liveliness GET drains 160 inline replies")

            status = api("rules", {"ingress": [{"pattern": "*/test", "active_endpoints": ["/stale"]}]})["status"]
            assert status["ingress"][0]["active_endpoints"] == [], status["ingress"]
            print("PASS: reapplying ingress does not report stale endpoints")

            key = "test/smoke/text"
            api("zenoh/sniff/start", {"key_expr": key})
            api("zenoh/put", {"key_expr": key, "payload": "a" * 199 + "é"})
            def received_preview():
                samples = api("zenoh/sniff")["samples"]
                if not samples:
                    return False
                preview = samples[-1]["payload_preview"]
                assert preview == "a" * 199 + "...", preview
                return True
            eventually(received_preview, 3)
            api("zenoh/sniff/stop", {})
            print("PASS: UTF-8 boundary in sniffer returns valid JSON")

            def check_info():
                info = api("zenoh/info")
                if info["zid"]:  # Reconnect may briefly leave the node disconnected.
                    configured = json.loads(info["session_config"]["raw_json5"])["id"]
                    assert info["zid"] == info["effective_config"]["id"] == configured, info
                return info["zid"]

            def update_while_reading(path, body, read, expected):
                with concurrent.futures.ThreadPoolExecutor(1) as pool:
                    writer = pool.submit(api, path, {**body, "save": False})
                    while not writer.done():
                        read()
                        time.sleep(0.01)
                    writer.result()
                    assert read() == expected

            for zid in ("b2", "a1", "b2"):
                update_while_reading("zenoh/session", {"session": session(zid, listen_port)}, check_info, zid)
            print("PASS: session inspection stays consistent across reconnects")

            def check_identity():
                info = api("zenoh/info")
                settings = info["bridge_settings"]
                agent = settings["agent_id"] or info["default_agent_id"]
                assert info["agent_id"] == agent, (info["agent_id"], agent)
                return agent

            settings = api("zenoh/info")["bridge_settings"]
            for agent in ("renamed", "regression"):
                update_while_reading("bridge/settings", {"bridge_settings": {**settings, "agent_id": agent}},
                                     check_identity, agent)
            print("PASS: agent identity matches bridge settings during rename")
            if concurrency:
                check_rename_bindings(api, env)
                check_pending_shutdown(api, proc, binary, directory, listen_port, env, log)
        except BaseException:
            log.seek(0)
            print(log.read(), file=sys.stderr)
            raise
        finally:
            if proc.poll() is None:
                proc.send_signal(signal.SIGINT)
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()
                    raise RuntimeError("node failed to shut down")


if __name__ == "__main__":
    with tempfile.TemporaryDirectory() as tmp:
        run(str(Path(sys.argv[1]).resolve(strict=True)), Path(tmp), '--concurrency' in sys.argv[2:])
