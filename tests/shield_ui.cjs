// Node-only browser fixture: run with `node --test tests/shield_ui.cjs`.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');

test('polls merge drafts with current policy and writes never pin automatic domains', async () => {
  const elements = new Map();
  function element(id) {
    if (!elements.has(id)) elements.set(id, {
      id, value: '', checked: false, handlers: {},
      addEventListener(event, fn) { this.handlers[event] = fn; },
      classList: { add() {}, remove() {}, toggle() {} }, querySelectorAll() { return []; }, appendChild() {},
    });
    return elements.get(id);
  }
  const writes = [];
  const agent = { agent_id: 'local', is_local: true, online: true, dds_shield_enabled: false,
    dds_shield_token_required: true, dds_shield_management_available: true,
    dds_shield_active: false, dds_shield_scope: 'domain', dds_shield_configured_domains: '', dds_shield_domains: '7' };
  let infoReads = 0;
  let serveHtml = false;
  let lastHeaders;
  const page = { location: { href: 'https://example.invalid/app_zenoh_2026042116_ba01/ui/' }, isSecureContext: false };
  async function fetch(url, options) {
    assert.ok(url.startsWith('/zenoh/api/'), url);
    assert.equal(options.cache, 'no-store', 'an API URL can already have a cached HTML fallback');
    if (serveHtml && url.endsWith('/agents')) return new Response('<html></html>', {
      headers: { 'Content-Type': 'text/html' },
    });
    if (url.includes('/zenoh/info?')) {
      ++infoReads;
      return { ok: true, json: async () => ({ ok: true, zid: 'test-zid' }) };
    }
    if (url.endsWith('/agents')) return { ok: true, json: async () => ({ ok: true, local_agent_id: 'local', agents: [{ ...agent }] }) };
    assert.ok(url.endsWith('/dds_shield'));
    lastHeaders = options.headers;
    writes.push(JSON.parse(options.body));
    return { ok: true, json: async () => ({ ok: true }) };
  }
  vm.runInNewContext(fs.readFileSync(require.resolve('../ui/app.js'), 'utf8'), {
    document: { getElementById: element, createElement: id => element(id), querySelectorAll: () => [] },
    URL, AbortController, window: page,
    fetch, setTimeout() {}, clearTimeout() {}, setInterval() {},
  });
  const click = async id => element(id).handlers.click();
  const poll = () => click('btnRefreshFleet');
  await poll();
  await Promise.all([click('btnRefreshInfo'), click('btnRefreshInfo')]);
  assert.equal(infoReads, 1);
  assert.equal(element('infoOwnZid').textContent, 'test-zid');
  assert.equal(element('coreShieldDomains').value, '');
  element('coreShieldEnabled').checked = true;
  element('shieldToken').value = 'test';
  await click('btnCoreShieldApply');
  assert.equal(lastHeaders['X-DDS-Shield-Token'], 'test');
  assert.deepEqual(writes.pop(), { agent_id: 'local', enabled: true, save: true,
    expected_policy: { enabled: false, scope: 'domain', domains: '' } });

  // Another admin enables filtering while this user has an unsaved domain edit.
  element('coreShieldEnabled').checked = false; // pristine against the last poll
  agent.dds_shield_enabled = agent.dds_shield_active = true;
  element('coreShieldDomains').value = '9';
  await poll();
  assert.equal(element('coreShieldEnabled').checked, true);
  assert.equal(element('coreShieldDomains').value, '9');
  assert.match(element('coreShieldStatus').textContent, /域 7 /);
  element('shieldToken').value = 'test';
  await click('btnCoreShieldApply');
  assert.deepEqual(writes.pop(), { agent_id: 'local', domains: '9', save: true,
    expected_policy: { enabled: true, scope: 'domain', domains: '' } });

  // A conflicting edit to this same field is not silently replayed over the new policy.
  agent.dds_shield_configured_domains = agent.dds_shield_domains = '12';
  await poll();
  assert.equal(element('coreShieldDomains').value, '12');
  element('coreShieldDomains').value = '3';
  element('shieldToken').value = 'test';
  await click('btnDdsShieldPill');
  assert.deepEqual(writes.pop(), { agent_id: 'local', enabled: false, save: true,
    expected_policy: { enabled: true, scope: 'domain', domains: '12' } });
  page.location.href = 'http://127.0.0.1/zenoh/ui/';
  agent.dds_shield_token_required = false;
  await poll();
  assert.equal(element('shieldToken').hidden, true);
  assert.equal(element('btnCoreShieldApply').disabled, false);
  await click('btnCoreShieldApply');
  assert.equal(lastHeaders['X-DDS-Shield-Token'], undefined);
  assert.equal(writes.pop().agent_id, 'local');

  page.location.href = 'http://192.0.2.1/app_zenoh_2026042116_ba01/ui/';
  page.parent = { location: { origin: 'http://192.0.2.1' } };
  agent.dds_shield_token_required = true;
  await poll();
  assert.equal(element('btnCoreShieldApply').disabled, false);
  assert.equal(element('shieldToken').hidden, false);
  assert.match(element('coreShieldStatus').textContent, /ZENOH_BRIDGE_SHIELD_TOKEN/);

  agent.is_local = false;
  await poll();
  assert.equal(element('btnCoreShieldApply').disabled, true);
  assert.match(element('coreShieldStatus').textContent, /远端只读/);
  agent.is_local = true;
  agent.online = false;
  await poll();
  assert.equal(element('btnCoreShieldApply').disabled, true);
  assert.match(element('coreShieldStatus').textContent, /离线/);

  serveHtml = true;
  await poll();
  assert.match(element('localSummary').textContent, /\/zenoh\/api\/agents.*HTTP 200.*JSON.*text\/html/);
});
