"""Isolated export/import HTTP regression. Run after sourcing ROS 2 with the built binary as argv[1]."""
import contextlib
import copy
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
import urllib.error
import urllib.request

from bridge_smoke import eventually, local_api, session, stage_node


def run(binary, directory):
    binary = stage_node(binary, directory, """#!/bin/sh
[ "$1" = "status" ] || exit 1
printf '%s\\n' '{"ok":true,"active":false}'
""")
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    paths = {name: directory / filename for name, filename in (
        ("routes", "routes.yaml"), ("bridge_settings", "bridge_settings.json"),
        ("session_override", "session_override.json"))}
    settings = paths["bridge_settings"]
    settings.write_text(json.dumps({"dds_shield_scope": "domain", "dds_shield_domains": "17"}))
    runtime = directory / "runtime.json"
    runtime.write_text(json.dumps({
        "agent_id": "local", "robot_type": "uxrce_rotor", "session": session("ab01", 0),
        "rules_file": str(paths["routes"]), "bridge_settings_file": str(settings),
        "session_override_file": str(paths["session_override"]), "api_port": port,
    }))
    env = dict(os.environ, ROS_HOME=str(directory), ROS_DOMAIN_ID="117", ROS_LOCALHOST_ONLY="1",
               ROS_AUTOMATIC_DISCOVERY_RANGE="OFF", AIVUDA_APP_RUNTIME_DATA_PATH=str(directory))
    api = local_api(port, timeout=10)
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))

    def export():
        with opener.open(f"http://127.0.0.1:{port}/api/config/export", timeout=5) as response:
            return json.load(response)

    @contextlib.contextmanager
    def node():
        with (directory / "node.log").open("a+") as log:
            proc = subprocess.Popen([binary, "--ros-args", "-p", f"runtime_config_file:={runtime}"],
                                    env=env, stdout=log, stderr=subprocess.STDOUT)
            try:
                def ready():
                    if proc.poll() is not None:
                        raise RuntimeError(f"Node exited with {proc.returncode}")
                    try:
                        return api("health")["ok"]
                    except urllib.error.URLError:
                        return False
                eventually(ready, 15)
                yield
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
                        raise

    with node():
        api("rules", {"egress": [{"topic": "old"}, {"topic": "keep"}], "ingress": [{"pattern": "*/keep"}]})
        original = export()
        assert original["format_version"] == 1 and original["robot_type"] == "uxrce_rotor"
        assert set(original["routes"]) == {"egress", "ingress"}
        assert all("kbps" not in rule for rules in original["routes"].values() for rule in rules)
        assert all("shield" not in key for key in original["bridge_settings"])

        doc = copy.deepcopy(original)
        doc["routes"]["egress"].pop(0)
        doc["routes"]["egress"][0]["max_freq"] = 8.5
        doc["routes"]["egress"].append({"topic": "new", "history_depth": 2, "on_demand": True})
        doc["bridge_settings"]["allow_unprefixed_local"] = False
        doc["session_override"].update(session("ab02", 0))
        doc["agent_id"] = "copied"
        request = {"config": doc, "dry_run": True, "mode": "replace"}
        dry = api("config/import?agent_id=local", request)["summary"]
        assert [dry[k] for k in ("added", "modified", "deleted", "egress", "ingress")] == [1, 1, 1, 2, 1]
        assert dry["bridge_settings_changed"] and dry["session_override_changed"] and not dry["agent_id_changed"]
        assert export()["routes"] == original["routes"] and not paths["routes"].exists()
        assert not paths["session_override"].exists() and "agent_id" not in json.loads(settings.read_text())
        merge = api("config/import", {**request, "mode": "merge"})["summary"]
        assert merge["egress"] == 3 and merge["deleted"] == 0
        api("config/import?agent_id=other", request, expected=403)
        api("config/import", {**request, "mode": "wrong"}, expected=400)
        for invalid in ({"history_depth": 0}, {"topic": ""}, {"topic": "a space"},
                        {"max_freq": True}, {"priority": "bad"}, {"typo": False}):
            bad = copy.deepcopy(doc)
            bad["routes"]["egress"][0].update(invalid)
            api("config/import", {**request, "config": bad}, expected=400)
        bad = copy.deepcopy(doc)
        bad["bridge_settings"]["dds_shield_enabled"] = True
        api("config/import", {**request, "config": bad}, expected=400)
        bad = copy.deepcopy(doc)
        bad["routes"]["egress"].append({"topic": "/local/new"})
        api("config/import", {**request, "config": bad}, expected=400)
        assert export()["routes"] == original["routes"]

        applied = api("config/import", {**request, "dry_run": False})
        assert applied["summary"] == dry and applied["agent_id"] == "local"
        restored = export()
        assert restored["session_override"]["raw_json5"] == doc["session_override"]["raw_json5"]
        assert api("zenoh/info")["zid"] == "ab02"
        assert [r["topic"] for r in api("rules")["status"]["egress"]] == ["keep", "new"]
        assert restored["routes"]["egress"][1]["history_depth"] == 2
        assert not restored["bridge_settings"]["allow_unprefixed_local"]
        assert json.loads(settings.read_text())["dds_shield_domains"] == "17"
        assert all(p.is_file() for p in paths.values())
        # A malformed disk destination must roll back runtime as well.
        paths["routes"].rename(paths["routes"].with_suffix(".bak"))
        paths["routes"].mkdir()
        changes = copy.deepcopy(doc)
        changes["routes"]["egress"] = [{"topic": "failed"}]
        api("config/import", {"config": changes, "sections": ["routes"]}, expected=500)
        assert export()["routes"] == restored["routes"]
        paths["routes"].rmdir()
        paths["routes"].with_suffix(".bak").rename(paths["routes"])

        merged = api("config/import", {"config": original, "sections": ["routes"], "mode": "merge"})
        assert merged["summary"]["egress"] == 3 and not merged["summary"]["session_override_changed"]
        before_restart = export()
    # Simulate a power loss after the first of several files was replaced.
    previous = paths["routes"].read_text()
    journal = Path(str(settings) + ".import-journal")
    journal.write_text(json.dumps({"routes": {"path": str(paths["routes"]), "previous": previous}}))
    paths["routes"].write_text('egress: []\ningress: []\n')
    with node():
        assert not journal.exists() and export()["routes"] == before_restart["routes"]
        assert api("zenoh/info")["zid"] == "ab02"
        renamed = api("config/import", {"config": doc, "sections": [], "include_agent_id": True})
        assert renamed["agent_id"] == "copied" and export()["agent_id"] == "copied"
        assert json.loads(settings.read_text())["dds_shield_domains"] == "17"
    print("PASS: export, schema and identity checks, preview, merge/replace, live reload, rollback and restart")


if __name__ == "__main__":
    with tempfile.TemporaryDirectory() as temporary:
        run(str(Path(sys.argv[1]).resolve(strict=True)), Path(temporary))
