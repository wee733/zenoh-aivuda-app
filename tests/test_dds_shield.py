"""Exercise the actual helper and its nft CLI protocol without running nft or sudo."""
import copy
import importlib.util
import json
from pathlib import Path
from types import SimpleNamespace
import unittest
from unittest.mock import patch


SCRIPT = Path(__file__).resolve().parents[1] / 'scripts/dds_shield.py'
spec = importlib.util.spec_from_file_location('dds_shield', SCRIPT)
shield = importlib.util.module_from_spec(spec)
spec.loader.exec_module(shield)


class MockNft:
    """Model list and atomic transactions only; reject all other commands."""
    def __init__(self):
        self.tables = {}
        self.handle = 0
        self.batches = []
        self.reject = False

    def __call__(self, args, *, input, **kwargs):
        assert args[:4] == ['/mock/nft', '-j', '-n', '-a']
        args = args[4:]
        if args == ['list', 'tables', 'inet']:
            data = [records[0] for records in self.tables.values()]
        elif args[:3] == ['list', 'table', 'inet']:
            if args[3] not in self.tables:
                return SimpleNamespace(returncode=1, stderr='table not found')
            data = self.tables[args[3]]
        elif args == ['-f', '-']:
            batch = json.loads(input)['nftables']
            self.batches.append(batch)
            tables = copy.deepcopy(self.tables)
            for item in batch:
                verb, record = next(iter(item.items()))
                kind, obj = next(iter(record.items()))
                if verb == 'delete' and kind == 'chain':
                    markers = [r for r in tables.get(obj['table'], []) if r.get('chain') == obj]
                    if len(markers) != 1:
                        return SimpleNamespace(returncode=1, stderr='marker missing')
                    tables[obj['table']].remove(markers[0])
                elif verb == 'delete':
                    assert kind == 'table' and set(obj) == {'family', 'handle'} and obj['family'] == 'inet'
                    old = next((t for t, v in tables.items() if v[0]['table']['handle'] == obj['handle']), None)
                    if old is None:
                        return SimpleNamespace(returncode=1, stderr='stale handle')
                    del tables[old]
                elif verb == 'create':
                    assert kind == 'table' and obj['family'] == 'inet'
                    if obj['name'] in tables:
                        return SimpleNamespace(returncode=1, stderr='table exists')
                    self.handle += 1
                    tables[obj['name']] = [{'table': dict(obj, handle=self.handle)}]
                else:
                    assert verb == 'add' and kind in ('chain', 'rule') and obj['family'] == 'inet'
                    for expr in obj.get('expr', []):
                        if 'counter' in expr:
                            expr['counter'] = {'packets': 3, 'bytes': 212}
                    tables[obj['table']].append(record)
            if self.reject:
                return SimpleNamespace(returncode=1, stderr='batch rejected')
            self.tables = tables
            data = []
        else:
            raise AssertionError(f'Unexpected command: {args}')
        return SimpleNamespace(returncode=0, stdout=json.dumps({'nftables': data}))


class TestShield(unittest.TestCase):
    A = '/inst/A/settings.json'
    B = '/inst/B/settings.json'

    def setUp(self):
        self.nft = MockNft()
        for p in [patch.object(shield.subprocess, 'run', side_effect=self.nft),
                  patch.object(shield.shutil, 'which', return_value='/mock/nft'),
                  patch.object(shield.os, 'geteuid', return_value=0)]:
            p.start()
            self.addCleanup(p.stop)

    def call(self, cmd, scope='domain', csv='0', path=A):
        return shield.run([cmd, scope, csv, path])

    def test_rules_and_verified_counters(self):
        self.assertEqual(self.call('enable', 'all', '232, 1,1')['effective_domains'], '1,232')
        records, = self.nft.tables.values()
        for hook, iface in [('input', 'iifname'), ('output', 'oifname')]:
            rules = [r['rule']['expr'] for r in records if r.get('rule', {}).get('chain') == hook]
            self.assertEqual(len(rules), 6)
            self.assertEqual(rules[0], [{'match': {'op': '==', 'left': {'meta': {'key': iface}},
                                                   'right': 'lo'}}, {'return': None}])
            scout = rules[1]
            self.assertEqual([e['match']['right'] for e in scout[:-1]], ['224.0.0.224', 7446])
            self.assertEqual([e['match']['left']['payload'] for e in scout[:-1]],
                             [{'protocol': 'ip', 'field': 'daddr'}, {'protocol': 'udp', 'field': 'dport'}])
            self.assertEqual(scout[-1], {'return': None})
            for rule, proto, net, length in zip(rules[2:4], ['ip', 'ip6'], ['224.0.0.0', 'ff00::'], [4, 8]):
                multicast, ports = [e['match'] for e in rule[:2]]
                self.assertEqual(multicast['left']['payload'], {'protocol': proto, 'field': 'daddr'})
                self.assertEqual(multicast['right']['prefix'], {'addr': net, 'len': length})
                self.assertEqual(ports['right']['set'], [7400 + 250*d + k for d in range(233) for k in (0, 1)])
            self.assertEqual([r[0]['match']['right']['range'] for r in rules[4:]], [[7650, 7899], [65400, 65535]])
            for r in rules[2:]:
                self.assertEqual(r[-3]['match']['left']['payload'], {'protocol': 'udp', 'field': 'dport'})
                self.assertEqual(r[-1], {'drop': None})
            # Exercise alternate nft JSON formatting for port membership tests.
            for r in rules[2:]:
                m = r[-3]['match']
                m['op'] = 'in'
                if 'set' in m['right']:
                    m['right']['set'].reverse()
        with patch.object(shield, 'nft', wraps=shield.nft) as nft:
            status = self.call('status', 'all', '1,232')
            nft.assert_called_once_with('list', 'table', 'inet', next(iter(self.nft.tables)))
        self.assertTrue(status['active'])
        self.assertEqual(status['effective_domains'], '1,232')
        self.assertEqual(status['rx_dropped_packets'], 12)
        self.assertEqual(status['tx_dropped_bytes'], 848)
        with self.assertRaisesRegex(shield.ShieldError, 'does not match'):
            self.call('status')

    def test_snapshot_uses_verified_installed_policy(self):
        self.assertFalse(self.call('snapshot')['active'])
        for scope in ('all', 'domain'):
            self.call('enable', scope, '232,0,21')
            before = copy.deepcopy(self.nft.tables)
            snap = self.call('snapshot', 'domain', '111')
            self.assertEqual((snap['active'], snap['scope'], snap['effective_domains']),
                             (True, scope, '0,21,232'))
            self.assertEqual(self.nft.tables, before)
        records, = self.nft.tables.values()
        records[-1]['rule']['expr'][-1] = {'return': None}
        with self.assertRaisesRegex(shield.ShieldError, 'does not match'):
            self.call('snapshot')

    def test_bounded_nft(self):
        with patch.object(shield.subprocess, 'run', side_effect=shield.subprocess.TimeoutExpired('nft', 2)) as run:
            with self.assertRaisesRegex(shield.ShieldError, 'timed out'):
                self.call('snapshot')
            self.assertGreater(run.call_args.kwargs['timeout'], 0)

    def test_absent_status_and_disable_do_not_build_rules(self):
        with patch.object(shield, 'policy', side_effect=AssertionError('unneeded rule construction')):
            self.assertFalse(self.call('status')['active'])
            self.assertFalse(self.call('disable')['active'])
        self.assertEqual(self.nft.batches, [])

    def test_independent_owners_and_atomic_updates(self):
        self.assertFalse(self.call('status')['active'])
        self.call('enable')
        self.call('enable', path=self.B)
        a, b = self.nft.tables
        b_before = copy.deepcopy(self.nft.tables[b])
        self.call('enable', csv='2', path='/inst/A/./settings.json')
        batch = self.nft.batches[-1]
        self.assertTrue(batch[0]['delete']['chain']['name'].startswith('owner_v1_'))
        self.assertEqual(batch[1], {'delete': {'table': {'family': 'inet', 'handle': 1}}})
        self.assertEqual(batch[2], {'create': {'table': {'family': 'inet', 'name': a}}})
        self.assertTrue(self.call('status', csv='2')['active'])
        self.nft.reject = True
        with self.assertRaisesRegex(shield.ShieldError, 'batch rejected'):
            self.call('enable', 'all', '3')
        self.assertTrue(self.call('status', csv='2')['active'])
        self.nft.reject = False
        self.call('disable')
        self.call('disable')
        self.assertEqual(len(self.nft.batches), 5)
        self.assertEqual(self.nft.tables, {b: b_before})
        self.assertTrue(self.call('status', path=self.B)['active'])

    def test_foreign_incompatible_or_unreadable_table(self):
        self.call('enable')
        records, = self.nft.tables.values()
        records.pop(1)  # Exact table name, but no matching owner marker.
        for cmd in ('status', 'enable', 'disable'):
            with self.assertRaisesRegex(shield.ShieldError, 'ownership mismatch'):
                self.call(cmd)
        with patch.object(shield.subprocess, 'run', return_value=SimpleNamespace(returncode=1, stderr='denied')):
            with self.assertRaisesRegex(shield.ShieldError, 'denied'):
                self.call('disable')
        self.assertEqual(len(self.nft.batches), 1)

    def test_stale_owner_snapshot_cannot_delete_replacement(self):
        self.call('enable')
        records, = self.nft.tables.values()
        original_inspect = shield.inspect

        def concurrent_replacement(table, marker):
            result = original_inspect(table, marker)
            records[0]['table']['handle'] = 782  # Same name and marker, different table.
            return result

        with patch.object(shield, 'inspect', side_effect=concurrent_replacement):
            with self.assertRaisesRegex(shield.ShieldError, 'stale handle'):
                self.call('disable')
        self.assertTrue(self.call('status')['active'])  # Marker deletion also rolled back.

        def concurrent_owner_change(table, marker):
            result = original_inspect(table, marker)
            records[1]['chain']['name'] = 'owner_v1_other'
            return result

        with patch.object(shield, 'inspect', side_effect=concurrent_owner_change):
            with self.assertRaisesRegex(shield.ShieldError, 'marker missing'):
                self.call('enable')
        self.assertEqual(records[1]['chain']['name'], 'owner_v1_other')

    def test_status_rejects_tampered_rules_or_hooks(self):
        self.call('enable')
        records, = self.nft.tables.values()
        hook = next(r['chain'] for r in records if r.get('chain', {}).get('hook') == 'input')
        hook['hook'] = 'forward'
        with self.assertRaisesRegex(shield.ShieldError, 'does not match'):
            self.call('status')
        hook['hook'] = 'input'
        records[-1]['rule']['expr'][-1] = {'return': None}
        with self.assertRaisesRegex(shield.ShieldError, 'does not match'):
            self.call('status')

    def test_validate_before_privileged_commands(self):
        invalid = ['', ' ', '1,,2', ',0', '1,', '1;2', '1 2', '-1', '233', '1x', '１', '+2', '2\n']
        with patch.object(shield.subprocess, 'run', side_effect=AssertionError('unexpected command')):
            for csv in invalid:
                with self.subTest(csv=csv), self.assertRaises(shield.ShieldError):
                    self.call('enable', csv=csv)
            for args in [('invalid',), ('enable', 'bogus'), ('disable', 'domain', '0', 'relative'),
                         ('status', 'all', '0', '/')]:
                with self.assertRaises(shield.ShieldError):
                    self.call(*args)
            with self.assertRaises(shield.ShieldError):
                shield.run(['enable'])


if __name__ == '__main__':
    unittest.main()
