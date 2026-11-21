"""Manage the DDS UDP filter for one bridge installation."""
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys


class ShieldError(Exception):
    pass


class NftCommandError(ShieldError):
    pass


LEGACY = ("Only this installation's inet table is managed. On upgrade, review legacy "
          "ZENOH_DDS_SHIELD_IN/OUT separately; they are not inspected or removed.")


def arguments(argv):
    if len(argv) != 4:
        raise ShieldError("Expected: enable|disable|status|snapshot domain|all domain_csv absolute_settings_path")
    action, scope, csv, path = argv
    if action not in ('enable', 'disable', 'status', 'snapshot') or scope not in ('domain', 'all'):
        raise ShieldError("Unknown action or scope")
    tokens = [s.strip(' \t') for s in csv.split(',')]
    if not all(re.fullmatch(r'[0-9]{1,3}', s) and int(s) <= 232 for s in tokens):
        raise ShieldError("Expected a nonempty comma-separated list of decimal domain IDs (0..232)")
    if not os.path.isabs(path) or os.path.basename(path) in ('', '.', '..'):
        raise ShieldError("An absolute installation settings file path is required")
    # Resolve aliases of the installation directory, not the contents/inode of a
    # settings file which can be atomically replaced or not yet exist at startup.
    path = os.path.join(os.path.realpath(os.path.dirname(path)), os.path.basename(path))
    owner = hashlib.sha256(os.fsencode(path)).hexdigest()
    return action, scope, sorted(set(map(int, tokens))), 'zenoh_dds_' + owner[:32], 'owner_v1_' + owner


def nft(*args, batch=None):
    trusted_path = '/usr/sbin:/sbin:/usr/bin:/bin'
    binary = shutil.which('nft', path=trusted_path)
    if not binary:
        raise ShieldError("Requires nftables with JSON, inet hooks, and table-handle deletion")
    cmd = [binary, '-j', '-n', '-a', *args]
    if os.geteuid() != 0:
        sudo = shutil.which('sudo', path=trusted_path)
        if not sudo:
            raise ShieldError("Requires root or noninteractive sudo for nft")
        cmd = [sudo, '-n', '--', *cmd]
    data = None if batch is None else json.dumps({'nftables': batch})
    try:
        p = subprocess.run(cmd, input=data, text=True, capture_output=True, timeout=2)
    except subprocess.TimeoutExpired as exc:
        raise ShieldError('nft timed out') from exc
    if p.returncode:
        raise NftCommandError('nft failed: ' + (p.stderr.strip()[:500] or str(p.returncode)))
    if batch is None:
        records = json.loads(p.stdout)['nftables']
        if not isinstance(records, list) or not all(isinstance(r, dict) and len(r) == 1 for r in records):
            raise ShieldError('Unexpected nft listing')
        return [r for r in records if 'metainfo' not in r]


def match(left, right):
    return {'match': {'op': '==', 'left': left, 'right': right}}


def payload(proto, field, value):
    return match({'payload': {'protocol': proto, 'field': field}}, value)


def policy(table, marker, scope, domains):
    # Standard RTPS multicast discovery/data ports for supported domains.
    if scope == 'all':
        multicast = {'set': [7400 + 250*d + i for d in range(233) for i in (0, 1)]}
    base = {'family': 'inet', 'table': table}
    objects = [{'table': {'family': 'inet', 'name': table}}, {'chain': dict(base, name=marker)}]
    for hook, iface in [('input', 'iifname'), ('output', 'oifname')]:
        objects.append({'chain': dict(base, name=hook, type='filter', hook=hook, prio=0, policy='accept')})

        def rule(*expr):
            objects.append({'rule': dict(base, chain=hook, expr=list(expr))})

        rule(match({'meta': {'key': iface}}, 'lo'), {'return': None})
        # 7446/7447 are also valid domain-0 participant ports. Do not exempt
        # source ports, all destinations on those ports, or the whole scouting IP.
        rule(payload('ip', 'daddr', '224.0.0.224'), payload('udp', 'dport', 7446), {'return': None})
        if scope == 'all':
            for proto, addr, length in [('ip', '224.0.0.0', 4), ('ip6', 'ff00::', 8)]:
                rule(payload(proto, 'daddr', {'prefix': {'addr': addr, 'len': length}}),
                     payload('udp', 'dport', multicast), {'counter': None}, {'drop': None})
        for d in domains:
            lo = 7400 + 250*d
            rule(payload('udp', 'dport', {'range': [lo, min(65535, lo + 249)]}),
                 {'counter': None}, {'drop': None})
    return objects


def inspect(table, marker):
    try:
        records = nft('list', 'table', 'inet', table)
    except NftCommandError:
        # Only an independent, successful listing can establish absence.
        tables = nft('list', 'tables', 'inet')
        if not all('table' in r and 'name' in r['table'] and 'family' in r['table'] for r in tables):
            raise ShieldError('Invalid nft table listing')
        if any(r['table']['family'] == 'inet' and r['table']['name'] == table for r in tables):
            raise
        return None, []
    tables = [r['table'] for r in records if 'table' in r]
    markers = [r['chain'] for r in records if r.get('chain', {}).get('name', '').startswith('owner_v')]
    if not (len(tables) == len(markers) == 1 and tables[0].get('family') == 'inet' and
            tables[0].get('name') == table and markers[0].get('family') == 'inet' and
            markers[0].get('table') == table and markers[0].get('name') == marker):
        raise ShieldError('Table ownership mismatch or incompatible version; refusing to manage it')
    handle = tables[0].get('handle')
    if type(handle) is not int or handle <= 0:
        raise ShieldError('Missing nft table handle; cannot guard against concurrent replacements')
    return handle, records


def checked_status(records, expected):
    # nft groups output by chain. Compare chain metadata and ordered statements,
    # normalizing handles, counter values and equivalent port membership syntax.
    counts = {hook: [0, 0] for hook in ('input', 'output')}

    def shape(objects, kernel):
        definitions, rules = {}, {}
        for record in objects:
            kind, obj = next(iter(record.items()))
            obj = obj.copy()
            obj.pop('handle', None)
            if kind == 'rule':
                chain = obj['chain']
                for expr in obj['expr']:
                    if 'counter' in expr and kernel:
                        c = expr['counter']
                        for i, key in enumerate(('packets', 'bytes')):
                            if type(c[key]) is not int or c[key] < 0:
                                raise ShieldError('Invalid nft counter')
                            counts[chain][i] += c[key]
                        expr['counter'] = None
                    m = expr.get('match', {})
                    rhs = m.get('right')
                    # For UDP port collections only, nft can spell membership
                    # as either == or in. Do not conflate scalar/bitmask matches.
                    if (m.get('left') == {'payload': {'protocol': 'udp', 'field': 'dport'}} and
                            isinstance(rhs, dict) and set(rhs) in ({'set'}, {'range'})):
                        if m.get('op') == 'in':
                            m['op'] = '=='
                        if 'set' in rhs:
                            rhs['set'] = sorted(rhs['set'])
                rules.setdefault(chain, []).append(obj)
            else:
                key = (kind, obj['name'])
                if key in definitions:
                    raise ShieldError('Duplicate nft object')
                definitions[key] = obj
        return definitions, rules

    if shape(records, True) != shape(expected, False):
        raise ShieldError('Owned table does not match requested policy; filtering state not verified')
    return counts


def infer_policy(records):
    # Infer a candidate from input rules, not configuration (which may
    # have changed since restart). Verify *both* hooks and the entire
    # ruleset below before accepting it as a restorable policy.
    scope, ids = 'domain', set()
    for record in records:
        rule = record.get('rule', {})
        if rule.get('chain') != 'input':
            continue
        for expr in rule['expr']:
            m = expr.get('match', {})
            if m.get('left') != {'payload': {'protocol': 'udp', 'field': 'dport'}}:
                continue
            rhs = m.get('right')
            if isinstance(rhs, dict) and 'set' in rhs:
                scope = 'all'
            if isinstance(rhs, dict) and 'range' in rhs:
                lo, _ = rhs['range']
                ids.add((lo - 7400) // 250)
    if not ids or min(ids) < 0 or max(ids) > 232:
        raise ShieldError('Cannot identify installed policy domains')
    return scope, sorted(ids)


def run(argv):
    action, scope, domains, table, marker = arguments(argv)
    handle, records = inspect(table, marker)
    counts = {'input': (0, 0), 'output': (0, 0)}
    active = handle is not None
    if action in ('snapshot', 'status'):
        if active:
            if action == 'snapshot':
                scope, domains = infer_policy(records)
            counts = checked_status(records, policy(table, marker, scope, domains))
    else:
        # Guard a prior ownership check by deleting the inspected handle, never a
        # potentially-reused name. Exclusive create catches name races. One kernel
        # transaction: an error cannot remove just the old rules or half the hooks.
        batch = [] if handle is None else [
            # Assert the owner marker is still present inside the transaction.
            {'delete': {'chain': {'family': 'inet', 'table': table, 'name': marker}}},
            {'delete': {'table': {'family': 'inet', 'handle': handle}}},
        ]
        active = action == 'enable'
        if active:
            expected = policy(table, marker, scope, domains)
            batch.extend({('create' if 'table' in obj else 'add'): obj} for obj in expected)
        if batch:
            nft('-f', '-', batch=batch)
    effective_domains = ','.join(map(str, domains)) if active else ''
    message = (f'Owned UDP destination-port filter installed (scope={scope}, domains={effective_domains})'
               if active else 'Owned table absent')
    return dict(ok=True, active=active, scope=scope, effective_domains=effective_domains, message=message + '. ' + LEGACY,
                rx_dropped_packets=counts['input'][0], rx_dropped_bytes=counts['input'][1],
                tx_dropped_packets=counts['output'][0], tx_dropped_bytes=counts['output'][1])


if __name__ == '__main__':
    try:
        print(json.dumps(run(sys.argv[1:])))
    except (ShieldError, OSError, ValueError, KeyError, TypeError, AttributeError) as exc:
        print(json.dumps(dict(ok=False, active=False, rx_dropped_packets=0, rx_dropped_bytes=0,
                              tx_dropped_packets=0, tx_dropped_bytes=0,
                              message='Filtering state not verified: ' + str(exc) + '. ' + LEGACY)))
        sys.exit(1)
