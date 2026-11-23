(() => {
  // Caddy serves the API here even when the Store assigns the UI a different app ID.
  const API_BASE = '/zenoh/api';

  const PRIORITIES = [
    'real_time',
    'interactive_high',
    'interactive_low',
    'data_high',
    'data',
    'data_low',
    'background',
  ];
  const CONGESTIONS = ['drop', 'block', 'block_first'];
  const RELIABILITIES = ['reliable', 'best_effort'];
  const LOCALITIES = ['any', 'remote', 'session_local'];
  const EGRESS_QOS = ['auto', 'best_effort', 'reliable', 'sensor_data', 'transient_local'];
  const INGRESS_QOS = ['auto', 'best_effort', 'reliable', 'transient_local'];

  const EGRESS_FIELDS = {
    'eg-enabled': 'enabled',
    'eg-topic': 'topic',
    'eg-zkey': 'zenoh_key',
    'eg-freq': 'max_freq',
    'eg-kbps': 'max_kbps',
    'eg-ondemand': 'on_demand',
    'eg-prio': 'priority',
    'eg-cong': 'congestion',
    'eg-express': 'express',
    'eg-rel': 'reliability',
    'eg-loc': 'locality',
    'eg-qos': 'qos',
    'eg-depth': 'history_depth',
  };
  const INGRESS_FIELDS = {
    'ig-enabled': 'enabled',
    'ig-pat': 'pattern',
    'ig-remap': 'remap_prefix',
    'ig-excl': 'exclude_self',
    'ig-freq': 'max_freq',
    'ig-loc': 'locality',
    'ig-qos': 'qos',
    'ig-depth': 'history_depth',
  };

  const state = {
    localAgentId: '',
    selectedAgentId: '',
    agents: [],
    draftEgress: [],
    draftIngress: [],
    dirty: false,
    sessionLoadedForAgent: '',
    configMode: 'structured', // 'structured' | 'raw_json5'
    lastEffectiveConfigText: '',
    sniffActive: false,
    activeTab: 'routes',
    logsCache: null,
  };

  const $ = (id) => document.getElementById(id);

  function toast(msg, type = 'ok') {
    const el = document.createElement('div');
    el.className = `toast ${type}`;
    el.textContent = msg;
    $('toastContainer').appendChild(el);
    setTimeout(() => el.remove(), 3800);
  }

  async function api(path, options = {}) {
    const resp = await fetch(`${API_BASE}${path}`, {
      ...options,
      cache: 'no-store',
      headers: { 'Content-Type': 'application/json', ...(options.headers || {}) },
    });
    const data = await resp.json().catch(() => ({
      ok: false,
      message: `${API_BASE}${path.split('?')[0]} 返回 HTTP ${resp.status}，响应不是有效的 JSON（${resp.headers.get('content-type') || '未知类型'}）`,
    }));
    if (!resp.ok && data.ok === undefined) {
      data.ok = false;
    }
    return data;
  }

  function setDirty(flag) {
    state.dirty = flag;
    if (flag) $('presetSelect').value = 'current';
    $('dirtyBadge').classList.toggle('hidden', !flag);
  }

  function escapeHtml(str) {
    return String(str ?? '')
      .replace(/&/g, '&amp;')
      .replace(/</g, '&lt;')
      .replace(/>/g, '&gt;')
      .replace(/"/g, '&quot;');
  }

  function makeSelect(options, value, cls, attrs = '') {
    return `<select class="input-select ${cls}" ${attrs}>${options
      .map((opt) => `<option value="${opt}" ${opt === value ? 'selected' : ''}>${opt}</option>`)
      .join('')}</select>`;
  }

  function getSelectedAgent() {
    return state.agents.find((a) => a.agent_id === state.selectedAgentId) || null;
  }

  let logRequestId = 0;
  let lastAppliedLogRequest = 0;

  function selectAgent(id) {
    if (state.selectedAgentId === id) return;
    state.selectedAgentId = id;
    $('presetSelect').value = 'current';
    $('shieldToken').value = '';
    invalidateSessionInfo();
    lastAppliedLogRequest = logRequestId;
    state.logsCache = null;
    renderLogs();
  }

  function setConfigMode(mode) {
    state.configMode = mode === 'raw_json5' ? 'raw_json5' : 'structured';
    const isRaw = state.configMode === 'raw_json5';
    $('btnModeStructured').classList.toggle('active', !isRaw);
    $('btnModeRawJson5').classList.toggle('active', isRaw);
    $('cfgModeStructured').classList.toggle('hidden', isRaw);
    $('cfgModeRawJson5').classList.toggle('hidden', !isRaw);
  }

  function showValidateBox(ok, text) {
    const box = $('cfgValidateBox');
    if (!text) {
      box.classList.add('hidden');
      box.textContent = '';
      return;
    }
    box.classList.remove('hidden', 'ok', 'err');
    box.classList.add(ok ? 'ok' : 'err');
    box.textContent = text;
  }

  // ---------------- Fleet & Polling ----------------

  let currentPoll;
  let queuedPoll;

  function pollAgents() {
    if (currentPoll) {
      queuedPoll ??= currentPoll.then(() => {
        queuedPoll = null;
        return pollAgents();
      });
      return queuedPoll;
    }
    currentPoll = refreshAgents().finally(() => { currentPoll = null; });
    return currentPoll;
  }

  async function refreshAgents() {
    const controller = new AbortController();
    const deadline = setTimeout(() => controller.abort(), 8000);
    try {
      const data = await api('/agents', { signal: controller.signal });
      if (!data.ok) throw new Error(data.message || 'API error');

      $('apiStatusDot').className = 'conn-dot ok';
      state.localAgentId = data.local_agent_id;
      state.agents = data.agents || [];

      const needsSelection = !state.selectedAgentId || !getSelectedAgent();
      if (needsSelection) {
        selectAgent(state.localAgentId || (state.agents[0]?.agent_id ?? ''));
      }

      const local = state.agents.find((a) => a.agent_id === state.localAgentId);
      if (local) {
        $('localSummary').textContent = `Local: ${local.agent_id} (${local.zenoh_mode}) · Prefix: ${local.domain_prefix} · ROS ${local.ros_distro}`;
        $('metricTx').textContent = `${(local.total_tx_kbps || 0).toFixed(1)} KB/s`;
        $('metricRx').textContent = `${(local.total_rx_kbps || 0).toFixed(1)} KB/s`;
        $('metricPeers').textContent = String((local.peer_zids?.length || 0) + (local.router_zids?.length || 0));
      }

      renderFleet();
      renderAgentBanner();

      if (needsSelection || !state.dirty) {
        loadDraftFromSelectedAgent(needsSelection);
      } else {
        updateLiveMetricCellsOnly();
      }

      renderDiscovery();
      if (state.activeTab === 'logs') {
        pollLogs();
      }
      if (state.activeTab === 'session' && state.sessionLoadedForAgent !== state.selectedAgentId) {
        loadZenohInfo(true);
      }
    } catch (e) {
      $('apiStatusDot').className = 'conn-dot err';
      $('localSummary').textContent = `无法连接后端 API: ${controller.signal.aborted ? '请求超时' : e.message}`;
      for (const agent of state.agents) agent.online = false;
      renderFleet();
      updateDdsShieldUi(getSelectedAgent());
      updateConfigButtons(getSelectedAgent());
    } finally {
      clearTimeout(deadline);
    }
  }

  function sameRuleFields(previous, current, fields) {
    return previous.length === current.length && previous.every((rule, i) =>
      fields.every((field) => rule[field] === current[i][field]));
  }

  function loadDraftFromSelectedAgent(force = true) {
    const ag = getSelectedAgent();
    if (!ag) return;
    const egress = ag.egress || [];
    const ingress = ag.ingress || [];
    const rebuildEgress = force || !sameRuleFields(state.draftEgress, egress, Object.values(EGRESS_FIELDS));
    const rebuildIngress = force || !sameRuleFields(state.draftIngress, ingress, Object.values(INGRESS_FIELDS));
    state.draftEgress = egress.map((rule) => ({ ...rule }));
    state.draftIngress = ingress.map((rule) => ({ ...rule }));
    setDirty(false);
    if (rebuildEgress) {
      renderEgressTable();
    } else {
      $('egressTbody').querySelectorAll('tr[data-idx]').forEach((row, i) => {
        const cell = row.querySelector('.eg-stat');
        cell.dataset.topic = egress[i].topic;
        cell.innerHTML = formatEgressStatHtml(egress[i]);
        row.querySelector('.resolved-type').textContent = egress[i].resolved_type || '等待本机发布...';
      });
    }
    if (rebuildIngress) {
      renderIngressTable();
    } else {
      $('ingressTbody').querySelectorAll('.ig-stat').forEach((cell, i) => {
        cell.dataset.pat = ingress[i].pattern;
        cell.innerHTML = formatIngressStatHtml(ingress[i]);
      });
    }
  }

  function renderFleet() {
    const list = $('fleetList');
    if (!state.agents.length) {
      list.innerHTML = '<div class="empty-hint">暂无在线节点</div>';
      return;
    }
    list.innerHTML = state.agents
      .map((ag) => {
        const active = ag.agent_id === state.selectedAgentId ? 'active' : '';
        const dot = ag.online ? 'dot-green' : 'dot-gray';
        const localTag = ag.is_local ? '<span class="badge badge-info">LOCAL</span>' : '';
        const shieldTag = ag.online && ag.dds_shield_active && !ag.dds_shield_error
          ? '<span class="badge badge-ok" title="节点报告 DDS 端口过滤已生效">🛡️</span>'
          : '';
        const errTag = (ag.error_count || 0) > 0
          ? `<span class="badge badge-err" title="存在 ${ag.error_count} 条错误日志">! ${ag.error_count}</span>`
          : '';
        return `
          <div class="fleet-card ${active}" data-agent="${escapeHtml(ag.agent_id)}">
            <div class="fleet-row">
              <div class="fleet-name"><span class="dot ${dot}"></span>${escapeHtml(ag.agent_id)}</div>
              <div class="btn-row" style="gap:4px">${shieldTag}${errTag}${localTag}</div>
            </div>
            <div class="fleet-meta">
              <span>${escapeHtml(ag.zenoh_mode)} · ${escapeHtml(ag.ros_distro || 'ros2')}</span>
              <span class="mono">${(ag.total_tx_kbps || 0).toFixed(1)}↑ ${(ag.total_rx_kbps || 0).toFixed(1)}↓</span>
            </div>
          </div>`;
      })
      .join('');

    list.querySelectorAll('.fleet-card').forEach((card) => {
      card.addEventListener('click', () => {
        const id = card.getAttribute('data-agent');
        if (id && id !== state.selectedAgentId) {
          selectAgent(id);
          loadDraftFromSelectedAgent();
          renderFleet();
          renderAgentBanner();
          renderDiscovery();
          if (state.activeTab === 'logs') pollLogs();
          if (state.activeTab === 'session') loadZenohInfo(true);
        }
      });
    });
  }

  function renderAgentBanner() {
    const ag = getSelectedAgent();
    updateDdsShieldUi(ag);
    updateConfigButtons(ag);
    if (!ag) return;
    const label = ag.agent_id + (ag.is_local ? ' (本机)' : ' (远端)');
    $('selectedAgentTitle').textContent = label;
    $('sessTargetAgent').textContent = label;
    $('selectedAgentMode').textContent = `mode: ${ag.zenoh_mode}`;
    $('selectedAgentDistro').textContent = `ROS ${ag.ros_distro || 'ros2'} · ${ag.hostname || ''}`;
    $('selectedAgentZid').textContent = ag.zid ? `ZID ${ag.zid.slice(0, 12)}...` : '';
    $('topicCountBadge').textContent = String((ag.local_topics || []).length);
    $('discAgentName').textContent = ag.agent_id;
    $('logsAgentName').textContent = ag.agent_id;
    const alerts = (ag.error_count || 0) + (ag.warn_count || 0);
    const lb = $('logAlertBadge');
    if (alerts > 0) {
      lb.textContent = String(alerts);
      lb.classList.remove('hidden');
      lb.classList.toggle('alert', (ag.error_count || 0) > 0);
    } else {
      lb.classList.add('hidden');
      lb.classList.remove('alert');
    }
  }

  let shieldFormAgent = '';
  let shieldBaseline = null;
  const shieldControls = { enabled: 'coreShieldEnabled', scope: 'coreShieldScope', domains: 'coreShieldDomains' };

  function readShieldField(key) {
    const input = $(shieldControls[key]);
    return key === 'enabled' ? input.checked : input.value;
  }

  function shieldEditReason(ag) {
    if (!ag) return '等待选择在线节点。';
    if (!ag.online) return '节点离线，请在连接恢复后重试。';
    if (!ag.is_local || ag.agent_id !== state.localAgentId) return '远端只读，请前往目标节点管理。';
    if (typeof ag.dds_shield_configured_domains !== 'string') return '此节点未提供可编辑的过滤配置。';
    if (ag.dds_shield_management_available === false) {
      return ag.dds_shield_token_required ? '此连接无法管理。请确认服务绑定地址与管理令牌。'
        : '免令牌管理需要从目标主机访问其本机网关。';
    }
    const url = new URL(window.location.href);
    const isLocal = ['localhost', '127.0.0.1', '[::1]'].includes(url.hostname);
    if (url.protocol === 'https:' || (url.protocol === 'http:' && isLocal)) return '';
    const appUi = /^\/(?:zenoh|app_[^/]+)\/ui(?:\/|$)/.test(url.pathname);
    if (url.protocol === 'http:' && appUi) {
      if (/\bElectron\//.test(window.navigator?.userAgent || '')) return '';
      try {
        if (window.parent && window.parent !== window && window.parent.location.origin === url.origin) return '';
      } catch (_) { /* A cross-origin parent cannot identify the local gateway. */ }
      if (ag.dds_shield_token_required === false && ag.dds_shield_management_available) return '';
    }
    return '请通过 HTTPS、本机回环地址或 AivudaOS 内嵌页面打开。';
  }

  function updateDdsShieldUi(ag) {
    const pill = $('btnDdsShieldPill');
    const txt = $('ddsShieldPillText');
    const bar = $('ddsShieldBar');
    const barStatus = $('coreShieldStatus');
    const infoEl = $('bsDdsShieldInfo');
    const reason = shieldEditReason(ag);
    for (const id of ['btnDdsShieldPill', 'coreShieldEnabled', 'coreShieldScope', 'coreShieldDomains', 'shieldToken', 'btnCoreShieldApply']) {
      $(id).disabled = Boolean(reason);
    }
    const requiresToken = ag?.dds_shield_token_required !== false;
    $('shieldToken').hidden = Boolean(reason) || !requiresToken;
    if ($('shieldToken').hidden) $('shieldToken').value = '';
    $('btnCoreShieldApply').title = reason;
    if (!ag) {
      barStatus.textContent = reason;
      txt.textContent = 'DDS 隔离：等待节点';
      shieldFormAgent = '';
      shieldBaseline = null;
      return;
    }

    const rx = ag.dds_shield_rx_dropped || 0;
    const tx = ag.dds_shield_tx_dropped || 0;
    const total = rx + tx;
    const scope = ag.dds_shield_scope === 'all' ? 'all' : 'domain';
    const scopeLabel = `域 ${ag.dds_shield_domains || '未知'} 标准 UDP 端口` + (scope === 'all' ? '及各域发现组播端口' : '');
    const active = ag.online && ag.dds_shield_active && !ag.dds_shield_error;
    pill.classList.remove('active', 'inactive', 'neutral');
    pill.classList.add(active ? 'active' : 'neutral');
    if (bar) bar.classList.toggle('active', Boolean(active));

    const policy = { enabled: Boolean(ag.dds_shield_enabled), scope,
      domains: ag.dds_shield_configured_domains ?? ag.dds_shield_domains ?? '' };
    const previous = shieldFormAgent === ag.agent_id ? shieldBaseline : null;
    let conflict = false;
    for (const [key, id] of Object.entries(shieldControls)) {
      // Polling preserves edits to fields whose live value hasn't changed.
      if (previous && policy[key] === previous[key]) continue;
      const draft = readShieldField(key);
      if (previous && draft !== previous[key] && draft !== policy[key]) conflict = true;
      $(id)[key === 'enabled' ? 'checked' : 'value'] = policy[key];
    }
    if (conflict) toast('其他操作修改了过滤策略，冲突项已更新。请核对后再应用。', 'err');
    shieldFormAgent = ag.agent_id;
    shieldBaseline = policy;

    const summary = !ag.online ? '节点离线，状态未知'
      : ag.dds_shield_error ? '过滤状态未确认'
      : active ? `过滤中（拦截 ${total} 包）`
      : ag.dds_shield_enabled ? '已配置，未检测到生效规则' : '本实例未启用';
    const details = ag.dds_shield_error || (active ? `${scopeLabel} · RX ${rx} / TX ${tx}` : '');
    const access = reason || (requiresToken
      ? '请输入本机桥服务环境变量 ZENOH_BRIDGE_SHIELD_TOKEN 的值。'
      : '当前本机连接可直接应用。');
    txt.textContent = `🛡️ ${ag.agent_id}: ${summary}`;
    pill.title = `${summary} ${details}。${access}`;
    if (barStatus) barStatus.textContent = [summary, details, access].filter(Boolean).join(' · ');
    if (infoEl) infoEl.textContent = `DDS 端口过滤：${summary}`;
  }

  async function applyDdsShield(agentId, patch) {
    const agent = getSelectedAgent();
    if (shieldEditReason(agent) || agent.agent_id !== agentId || shieldFormAgent !== agentId) return;
    const token = $('shieldToken').value;
    const requiresToken = agent.dds_shield_token_required !== false;
    if (requiresToken && !token) {
      toast('请在「路由与流控」页输入本机的 DDS 过滤管理令牌', 'err');
      $('shieldToken').focus();
      return;
    }
    $('shieldToken').value = '';
    let res;
    try {
      res = await api('/dds_shield', {
        method: 'POST', redirect: 'error',
        headers: requiresToken ? { 'X-DDS-Shield-Token': token } : {},
        body: JSON.stringify({ agent_id: agentId, ...patch, expected_policy: shieldBaseline, save: true }),
      });
    } catch (e) {
      toast(`未收到防火墙操作结果，请刷新状态：${e.message}`, 'err');
      return;
    }
    if (res.ok) {
      // Accept server normalization (e.g. "09,2" -> "2,9") without replacing edits made in flight.
      if (state.selectedAgentId === agentId && 'domains' in patch &&
          readShieldField('domains') === patch.domains && typeof res.domains === 'string') {
        $('coreShieldDomains').value = res.domains;
      }
      toast(`${agentId}: ${res.message || 'DDS 过滤策略已更新'}`, 'ok');
      await pollAgents();
    } else {
      toast(`${agentId} DDS 隔离锁操作失败: ${res.message || '未知错误'}`, 'err');
      await pollAgents();
    }
  }

  async function toggleDdsShieldForSelected() {
    const ag = getSelectedAgent();
    if (!ag) return;
    // The shortcut only toggles; it must not apply unrelated unsaved form fields.
    await applyDdsShield(ag.agent_id, { enabled: !ag.dds_shield_active });
  }

  async function applyCoreShieldBar() {
    const ag = getSelectedAgent();
    if (!ag || !shieldBaseline) return;
    const patch = {};
    for (const key of Object.keys(shieldControls)) {
      const value = readShieldField(key);
      if (value !== shieldBaseline[key]) patch[key] = value;
    }
    // With no edits, Apply explicitly reconciles and saves the current policy.
    if (!Object.keys(patch).length) patch.enabled = shieldBaseline.enabled;
    await applyDdsShield(ag.agent_id, patch);
  }

  // ---------------- Egress & Ingress Tables ----------------

  function formatEgressStatHtml(r) {
    const subBadge = r.has_subscribers
      ? '<span class="badge badge-ok">有订阅者</span>'
      : '<span class="badge badge-muted">无订阅者</span>';
    const noDemandHint = (!r.on_demand && !r.has_subscribers && (r.actual_hz || 0) > 0)
      ? ' <span class="badge badge-warn" title="未勾选按需：本地仍在执行 put()，勾选按需可降至 0">未开按需</span>'
      : '';
    const errHtml = r.last_error
      ? `<div class="err-inline" title="${escapeHtml(r.last_error)}">⚠ ${escapeHtml(r.last_error)}</div>`
      : '';
    return `<div>${(r.actual_hz || 0).toFixed(1)} Hz · ${(r.kbps || 0).toFixed(1)} KB/s</div>` +
           `<div>TX ${r.tx_count || 0} / Drop ${r.dropped_count || 0} ${subBadge}${noDemandHint}</div>${errHtml}`;
  }

  function formatIngressStatHtml(r) {
    const eps = (r.active_endpoints || [])
      .slice(0, 3)
      .map((ep) => `<span class="badge badge-muted mono">${escapeHtml(ep)}</span>`)
      .join(' ');
    const errHtml = r.last_error
      ? `<div class="err-inline" title="${escapeHtml(r.last_error)}">⚠ ${escapeHtml(r.last_error)}</div>`
      : '';
    return `<div>${(r.actual_hz || 0).toFixed(1)} Hz · ${(r.kbps || 0).toFixed(1)} KB/s · RX ${r.rx_count || 0}</div>` +
           `<div>${eps || (r.last_error ? '' : '<span class="muted">等待远端数据...</span>')}</div>${errHtml}`;
  }

  function renderEgressTable() {
    const tbody = $('egressTbody');
    if (!state.draftEgress.length) {
      tbody.innerHTML = '<tr><td colspan="15" class="empty-hint">暂无出站规则，点击「+ 添加出站规则」或前往「话题发现」一键添加</td></tr>';
      return;
    }

    tbody.innerHTML = state.draftEgress
      .map((r, idx) => {
        const boundNote = r.resolved_type
          ? `<div class="muted resolved-type" style="font-size:10px">${escapeHtml(r.resolved_type)}</div>`
          : '<div class="muted resolved-type" style="font-size:10px">等待本机发布...</div>';
        return `
          <tr data-idx="${idx}">
            <td style="text-align:center"><input type="checkbox" class="eg-enabled" ${r.enabled ? 'checked' : ''} /></td>
            <td>
              <input type="text" class="input-text mono cell-input eg-topic" value="${escapeHtml(r.topic)}" placeholder="fmu/out/vehicle_odometry" />
              ${boundNote}
            </td>
            <td><input type="text" class="input-text mono cell-input eg-zkey" value="${escapeHtml(r.zenoh_key || '')}" placeholder="默认 swarm/data/..." /></td>
            <td><input type="number" class="input-num eg-freq" value="${Number(r.max_freq ?? 30)}" min="0" step="1" /></td>
            <td><input type="number" class="input-num eg-kbps" value="${Number(r.max_kbps ?? 0)}" min="0" step="10" /></td>
            <td style="text-align:center"><input type="checkbox" class="eg-ondemand" ${r.on_demand ? 'checked' : ''} /></td>
            <td>${makeSelect(PRIORITIES, r.priority || 'data_high', 'eg-prio cell-input')}</td>
            <td>${makeSelect(CONGESTIONS, r.congestion || 'drop', 'eg-cong cell-input')}</td>
            <td style="text-align:center"><input type="checkbox" class="eg-express" ${r.express ? 'checked' : ''} /></td>
            <td>${makeSelect(RELIABILITIES, r.reliability || 'reliable', 'eg-rel cell-input')}</td>
            <td>${makeSelect(LOCALITIES, r.locality || 'any', 'eg-loc cell-input')}</td>
            <td>${makeSelect(EGRESS_QOS, r.qos || 'auto', 'eg-qos cell-input')}</td>
            <td><input type="number" class="input-num eg-depth" value="${Number(r.history_depth ?? 10)}" min="1" max="1000" style="width:52px" /></td>
            <td class="stat-cell eg-stat" data-topic="${escapeHtml(r.topic)}">
              ${formatEgressStatHtml(r)}
            </td>
            <td><button class="btn btn-xs btn-danger eg-del" title="删除">✕</button></td>
          </tr>`;
      })
      .join('');
  }

  function renderIngressTable() {
    const tbody = $('ingressTbody');
    if (!state.draftIngress.length) {
      tbody.innerHTML = '<tr><td colspan="10" class="empty-hint">暂无入站规则，点击「+ 添加入站规则」接收集群其他节点话题</td></tr>';
      return;
    }

    tbody.innerHTML = state.draftIngress
      .map((r, idx) => {
        return `
          <tr data-idx="${idx}">
            <td style="text-align:center"><input type="checkbox" class="ig-enabled" ${r.enabled ? 'checked' : ''} /></td>
            <td><input type="text" class="input-text mono cell-input ig-pat" value="${escapeHtml(r.pattern)}" placeholder="*/fmu/out/vehicle_odometry" /></td>
            <td><input type="text" class="input-text mono cell-input ig-remap" value="${escapeHtml(r.remap_prefix || '')}" placeholder="默认 /<源ID>/<话题>" /></td>
            <td style="text-align:center"><input type="checkbox" class="ig-excl" ${r.exclude_self !== false ? 'checked' : ''} /></td>
            <td><input type="number" class="input-num ig-freq" value="${Number(r.max_freq ?? 30)}" min="0" step="1" /></td>
            <td>${makeSelect(LOCALITIES, r.locality || 'any', 'ig-loc cell-input')}</td>
            <td>${makeSelect(INGRESS_QOS, r.qos || 'auto', 'ig-qos cell-input')}</td>
            <td><input type="number" class="input-num ig-depth" value="${Number(r.history_depth ?? 10)}" min="1" max="1000" style="width:54px" /></td>
            <td class="stat-cell ig-stat" data-pat="${escapeHtml(r.pattern)}">
              ${formatIngressStatHtml(r)}
            </td>
            <td><button class="btn btn-xs btn-danger ig-del" title="删除">✕</button></td>
          </tr>`;
      })
      .join('');
  }

  function updateLiveMetricCellsOnly() {
    const ag = getSelectedAgent();
    if (!ag) return;
    const egMap = new Map((ag.egress || []).map((r) => [r.topic, r]));
    $('egressTbody').querySelectorAll('.eg-stat').forEach((cell) => {
      const r = egMap.get(cell.getAttribute('data-topic'));
      if (r) cell.innerHTML = formatEgressStatHtml(r);
    });
    const igMap = new Map((ag.ingress || []).map((r) => [r.pattern, r]));
    $('ingressTbody').querySelectorAll('.ig-stat').forEach((cell) => {
      const r = igMap.get(cell.getAttribute('data-pat'));
      if (r) cell.innerHTML = formatIngressStatHtml(r);
    });
  }

  async function submitRules(save) {
    if (!state.selectedAgentId) return;
    const res = await api('/rules', {
      method: 'POST',
      body: JSON.stringify({
        agent_id: state.selectedAgentId,
        save,
        egress: state.draftEgress,
        ingress: state.draftIngress,
      }),
    });
    if (res.ok) {
      toast(`${state.selectedAgentId}: ${res.message || '规则已生效'}`, 'ok');
      setDirty(false);
      await pollAgents();
    } else {
      toast(`应用失败: ${res.message || '未知错误'}`, 'err');
    }
  }

  // ---------------- Topic Discovery ----------------

  function recommendRuleForTopic(topic, type) {
    const lower = `${topic} ${type}`.toLowerCase();
    const isHeavy = ['image', 'pointcloud', 'cloud', 'compressed', 'lidar', 'custommsg', 'scan', 'depth', 'camera', 'rgb']
      .some((k) => lower.includes(k));
    const isCritical = ['state', 'status', 'cmd', 'command'].some((k) => lower.includes(k));
    return {
      topic: topic.replace(/^\/+/, ''),
      type,
      zenoh_key: '',
      max_freq: isHeavy ? 2 : isCritical ? 10 : 30,
      max_kbps: 0,
      enabled: true,
      on_demand: isHeavy,
      priority: isCritical ? 'real_time' : isHeavy ? 'data_low' : 'data_high',
      congestion: 'drop',
      express: !isHeavy,
      reliability: isCritical ? 'reliable' : 'best_effort',
      locality: 'any',
      qos: 'auto',
      history_depth: isHeavy ? 5 : 10,
    };
  }

  function renderDiscovery() {
    if (state.activeTab !== 'discovery') return;
    const ag = getSelectedAgent();
    const tbody = $('discoveryTbody');
    if (!ag) {
      tbody.innerHTML = '';
      return;
    }
    const q = ($('topicSearchInput').value || '').trim().toLowerCase();
    const topics = (ag.local_topics || []).filter(
      (t) => !q || t.name.toLowerCase().includes(q) || t.type.toLowerCase().includes(q)
    );
    const existingSet = new Set(state.draftEgress.map((r) => r.topic.replace(/^\/+/, '')));

    if (!topics.length) {
      tbody.innerHTML = '<tr><td colspan="5" class="empty-hint">未发现匹配的在线 ROS 2 话题</td></tr>';
      return;
    }

    tbody.innerHTML = topics
      .map((t) => {
        const rel = t.name.replace(/^\/+/, '');
        const already = existingSet.has(rel) || existingSet.has(rel.replace(new RegExp(`^${ag.agent_id}/`), ''));
        const actionBtn = already
          ? '<span class="badge badge-ok">已在出站列表</span>'
          : `<button class="btn btn-xs btn-primary disc-add" data-name="${escapeHtml(t.name)}" data-type="${escapeHtml(t.type)}">+ 桥接到 Zenoh</button>`;
        return `
          <tr>
            <td class="mono">${escapeHtml(t.name)}</td>
            <td class="mono muted">${escapeHtml(t.type)}</td>
            <td class="mono">${t.pub_count || 0} / ${t.sub_count || 0}</td>
            <td><span class="badge badge-muted">${escapeHtml(t.reliability || '-')}</span> <span class="badge badge-muted">${escapeHtml(t.durability || '-')}</span></td>
            <td>${actionBtn}</td>
          </tr>`;
      })
      .join('');

    tbody.querySelectorAll('.disc-add').forEach((btn) => {
      btn.addEventListener('click', () => {
        const name = btn.getAttribute('data-name');
        const type = btn.getAttribute('data-type');
        state.draftEgress.push(recommendRuleForTopic(name, type));
        setDirty(true);
        renderEgressTable();
        renderDiscovery();
        toast(`已将 ${name} 加入出站草稿，点击左侧「应用当前节点」即可生效`, 'ok');
      });
    });
  }

  // ---------------- Zenoh Kernel & Bridge Settings ----------------

  const sessionFields = document.querySelectorAll('[data-session-key]');

  function parseLines(text) {
    return text.split(/\r?\n/).map((line) => line.trim()).filter(Boolean);
  }

  function populateSessionForm(config) {
    setConfigMode(config.config_mode || 'structured');
    for (const field of sessionFields) {
      const value = config[field.dataset.sessionKey];
      const fallback = field.dataset.default || '';
      if (field.type === 'checkbox') {
        field.checked = fallback === 'true' ? value !== false : Boolean(value);
      } else if (field.dataset.format === 'lines') {
        field.value = (value || []).join('\n');
      } else if (field.type === 'number') {
        field.value = value ?? fallback;
      } else {
        field.value = value || fallback;
      }
    }
  }

  function collectSessionConfigFromForm() {
    const config = { config_mode: state.configMode };
    for (const field of sessionFields) {
      const { sessionKey, format } = field.dataset;
      let value = field.value;
      if (field.type === 'checkbox') {
        value = field.checked;
      } else if (field.type === 'number') {
        const parsed = parseInt(value, 10);
        value = Number.isFinite(parsed) ? parsed : Number(field.dataset.default);
      } else if (format === 'lines') {
        value = parseLines(value);
      } else if (format !== 'raw' && field.type !== 'password' && field.tagName !== 'SELECT') {
        value = value.trim();
        if (sessionKey === 'multicast_interface' && !value) value = field.dataset.default;
      }
      config[sessionKey] = value;
    }
    return config;
  }

  function renderTransportsAndLinks(transports, links) {
    $('infoTransportCount').textContent = String((transports || []).length);
    const tBody = $('transportsTbody');
    if (!transports || !transports.length) {
      tBody.innerHTML = '<tr><td colspan="3" class="empty-hint">暂无活跃 Transport</td></tr>';
    } else {
      tBody.innerHTML = transports
        .map((t) => {
          const flags = [
            t.is_qos ? '<span class="badge badge-ok">QoS</span>' : '',
            t.is_multicast ? '<span class="badge badge-info">Multicast</span>' : '<span class="badge badge-muted">Unicast</span>',
            t.is_shm ? '<span class="badge badge-ok">SHM</span>' : '',
          ].filter(Boolean).join(' ');
          return `
            <tr>
              <td class="mono">${escapeHtml(t.zid)}</td>
              <td><span class="badge badge-info">${escapeHtml(t.whatami)}</span></td>
              <td>${flags}</td>
            </tr>`;
        })
        .join('');
    }

    $('infoLinkCount').textContent = String((links || []).length);
    const lBody = $('linksTbody');
    if (!links || !links.length) {
      lBody.innerHTML = '<tr><td colspan="3" class="empty-hint">暂无活跃 Link</td></tr>';
    } else {
      lBody.innerHTML = links
        .map((l) => {
          const ifaces = (l.interfaces || []).join(', ');
          const extra = [
            l.is_streamed ? '<span class="badge badge-muted">stream</span>' : '<span class="badge badge-muted">dgram</span>',
            l.reliability ? `<span class="badge badge-info">${escapeHtml(l.reliability)}</span>` : '',
            l.priorities ? `<span class="badge badge-muted">prio:${escapeHtml(l.priorities)}</span>` : '',
            ifaces ? `<span class="muted mono small">${escapeHtml(ifaces)}</span>` : '',
          ].filter(Boolean).join(' ');
          return `
            <tr>
              <td class="mono small">${escapeHtml(l.src)} &rarr; ${escapeHtml(l.dst)}</td>
              <td class="mono">${l.mtu || '-'}</td>
              <td>${extra}</td>
            </tr>`;
        })
        .join('');
    }
  }

  let infoRequest;
  function invalidateSessionInfo() {
    state.sessionLoadedForAgent = '';
    infoRequest = null;  // Ignore any response from before this selection or settings change.
  }

  function loadZenohInfo(forceFormReload = false) {
    const target = state.selectedAgentId || state.localAgentId;
    if (infoRequest?.target === target) {
      infoRequest.reload ||= forceFormReload;
      return infoRequest.promise;
    }
    const request = infoRequest = { target, reload: forceFormReload };
    request.promise = refreshZenohInfo(request).finally(() => {
      if (infoRequest === request) infoRequest = null;
    });
    return request.promise;
  }

  async function refreshZenohInfo(request) {
    const { target } = request;
    const qs = target ? `?agent_id=${encodeURIComponent(target)}` : '';
    const res = await api(`/zenoh/info${qs}`);
    if (infoRequest !== request) return;
    if (!res.ok) {
      toast(`获取 ${target || '节点'} 内核配置失败: ${res.message || '未知错误'}`, 'err');
      return;
    }

    $('infoOwnZid').textContent = res.zid || '--';
    $('infoPeerCount').textContent = String((res.peer_zids || []).length);
    $('infoRouterCount').textContent = String((res.router_zids || []).length);
    $('infoPeerZids').innerHTML = (res.peer_zids || []).length
      ? res.peer_zids.map((z) => `<span class="badge badge-ok mono">${escapeHtml(z)}</span>`).join('')
      : '<span class="muted small">无直连 Peer</span>';
    $('infoRouterZids').innerHTML = (res.router_zids || []).length
      ? res.router_zids.map((z) => `<span class="badge badge-info mono">${escapeHtml(z)}</span>`).join('')
      : '<span class="muted small">无连接 Router</span>';

    renderTransportsAndLinks(res.transports, res.links);

    const effText = typeof res.effective_config === 'string'
      ? res.effective_config
      : JSON.stringify(res.effective_config, null, 2);
    state.lastEffectiveConfigText = effText;
    $('effectiveConfigPre').textContent = effText;

    if (request.reload || state.sessionLoadedForAgent !== target) {
      if (res.default_agent_id) {
        $('bsDefaultAgentHint').textContent = res.default_agent_id;
      }
      if (res.bridge_settings) {
        const bs = res.bridge_settings;
        $('bsAgentId').value = bs.agent_id || '';
        $('bsDomainPrefix').value = bs.domain_prefix || 'swarm';
        $('bsDefaultPreset').value = bs.default_preset || 'empty';
        $('bsAllowUnprefixed').checked = bs.allow_unprefixed_local !== false;
        $('bsLiveliness').checked = bs.liveliness_enabled !== false;
      }
      if (res.session_config) {
        populateSessionForm(res.session_config);
      }
      showValidateBox(true, '');
      state.sessionLoadedForAgent = target;
    }
  }

  async function saveBridgeSettings() {
    const target = state.selectedAgentId || state.localAgentId;
    const payload = {
      agent_id: target,
      save: true,
      bridge_settings: {
        agent_id: $('bsAgentId').value.trim(),
        domain_prefix: $('bsDomainPrefix').value.trim() || 'swarm',
        default_preset: $('bsDefaultPreset').value,
        allow_unprefixed_local: $('bsAllowUnprefixed').checked,
        liveliness_enabled: $('bsLiveliness').checked,
      },
    };
    const res = await api('/bridge/settings', { method: 'POST', body: JSON.stringify(payload) });
    if (res.ok) {
      toast(res.message || '桥接设置已保存并热生效', 'ok');
      invalidateSessionInfo();
      if (target === state.localAgentId && res.agent_id) {
        selectAgent(res.agent_id);
      }
      setTimeout(async () => {
        await pollAgents();
        if (state.sessionLoadedForAgent !== state.selectedAgentId) await loadZenohInfo(true);
      }, 300);
    } else {
      toast(res.message || '保存桥接设置失败', 'err');
    }
  }

  async function validateSessionConfig() {
    const session = collectSessionConfigFromForm();
    const res = await api('/zenoh/validate_config', {
      method: 'POST',
      body: JSON.stringify({ session }),
    });
    if (res.ok) {
      showValidateBox(true, '✓ Zenoh 1.8.0 JSON5 配置校验通过！可安全热重连。');
      toast('配置校验通过', 'ok');
    } else {
      showValidateBox(false, `✕ 配置校验失败：\n${res.message || '未知错误'}`);
      toast('配置校验失败，请查看错误详情', 'err');
    }
    return res.ok;
  }

  async function reconnectSession() {
    const target = state.selectedAgentId || state.localAgentId;
    const session = collectSessionConfigFromForm();
    const res = await api('/zenoh/session', {
      method: 'POST',
      body: JSON.stringify({ agent_id: target, save: true, session }),
    });
    if (res.ok) {
      showValidateBox(true, `✓ ${res.message || 'Zenoh Session 已热重连'}`);
      toast(res.message || 'Zenoh Session 已热重连', 'ok');
      invalidateSessionInfo();
      setTimeout(async () => {
        await loadZenohInfo(true);
        await pollAgents();
      }, target === state.localAgentId ? 150 : 600);
    } else {
      showValidateBox(false, `✕ Session 重连失败：\n${res.message || '未知错误'}`);
      toast(res.message || 'Session 重连失败', 'err');
    }
  }

  // ---------------- Zenoh Lab: Scout, Liveliness, Sniffer ----------------

  async function runScout() {
    const tbody = $('scoutTbody');
    tbody.innerHTML = '<tr><td colspan="4" class="empty-hint">正在主动扫描局域网 Zenoh 节点...</td></tr>';
    const res = await api('/zenoh/scout', {
      method: 'POST',
      body: JSON.stringify({
        what: $('scoutWhat').value,
        timeout_ms: parseInt($('scoutTimeout').value, 10) || 1500,
      }),
    });
    if (!res.ok) {
      tbody.innerHTML = `<tr><td colspan="4" class="empty-hint">扫描失败: ${escapeHtml(res.message)}</td></tr>`;
      toast(res.message || 'Scout 失败', 'err');
      return;
    }
    const list = res.entities || [];
    if (!list.length) {
      tbody.innerHTML = '<tr><td colspan="4" class="empty-hint">未探测到任何 Zenoh 节点</td></tr>';
      return;
    }
    tbody.innerHTML = list
      .map((ent) => {
        const locs = ent.locators || [];
        const locBadges = locs.map((l) => `<span class="badge badge-muted mono">${escapeHtml(l)}</span>`).join(' ');
        const firstLoc = locs[0] || '';
        return `
          <tr>
            <td class="mono small">${escapeHtml(ent.zid)}</td>
            <td><span class="badge badge-info">${escapeHtml(ent.whatami)}</span></td>
            <td>${locBadges || '-'}</td>
            <td>${firstLoc ? `<button class="btn btn-xs btn-secondary scout-add" data-loc="${escapeHtml(firstLoc)}">+ Connect</button>` : '-'}</td>
          </tr>`;
      })
      .join('');

    tbody.querySelectorAll('.scout-add').forEach((btn) => {
      btn.addEventListener('click', () => {
        const loc = btn.getAttribute('data-loc');
        if (!loc) return;
        const cur = parseLines($('sessConnect').value);
        if (!cur.includes(loc)) {
          cur.push(loc);
          $('sessConnect').value = cur.join('\n');
        }
        toast(`已将 ${loc} 加入 Connect Endpoints（前往「Zenoh 内核配置」热重连生效）`, 'ok');
      });
    });
  }

  async function queryLiveliness() {
    const keyExpr = $('liveQueryKey').value.trim() || 'swarm/alive/**';
    const res = await api('/zenoh/liveliness/get', {
      method: 'POST',
      body: JSON.stringify({ key_expr: keyExpr, timeout_ms: 2000 }),
    });
    const tbody = $('liveTbody');
    if (!res.ok) {
      tbody.innerHTML = `<tr><td colspan="2" class="empty-hint">查询失败: ${escapeHtml(res.message)}</td></tr>`;
      toast(res.message || 'Liveliness 查询失败', 'err');
      return;
    }
    const tokens = res.tokens || [];
    if (!tokens.length) {
      tbody.innerHTML = '<tr><td colspan="2" class="empty-hint">当前无匹配的在线 Liveliness 令牌</td></tr>';
      return;
    }
    tbody.innerHTML = tokens
      .map((tk) => `
        <tr>
          <td class="mono">${escapeHtml(tk)}</td>
          <td><span class="badge badge-ok">ALIVE</span></td>
        </tr>`)
      .join('');
  }

  async function pollSniffer() {
    if (!state.sniffActive) return;
    try {
      const res = await api('/zenoh/sniff');
      if (!res.ok) return;
      state.sniffActive = Boolean(res.active);
      const tbody = $('sniffTbody');
      const list = res.samples || [];
      if (!list.length) {
        tbody.innerHTML = '<tr><td colspan="7" class="empty-hint">监听中，等待数据...</td></tr>';
        return;
      }
      tbody.innerHTML = list
        .map((s) => {
          const ts = new Date(s.timestamp_ms).toLocaleTimeString();
          const kindCls = s.kind === 'DELETE' ? 'badge-err' : 'badge-ok';
          return `
            <tr>
              <td class="mono muted">${ts}</td>
              <td><span class="badge ${kindCls}">${escapeHtml(s.kind || 'PUT')}</span></td>
              <td class="mono">${escapeHtml(s.key_expr)}</td>
              <td class="mono">${s.payload_bytes} B</td>
              <td class="mono muted">${escapeHtml(s.encoding)}</td>
              <td class="mono muted">${escapeHtml(s.attachment || '-')}</td>
              <td class="mono">${escapeHtml(s.payload_preview)}</td>
            </tr>`;
        })
        .join('');
    } catch (_) {}
  }

  // ---------------- Live Logs ----------------

  async function pollLogs() {
    const agentId = state.selectedAgentId;
    if (!agentId) return;
    const requestId = ++logRequestId;
    try {
      const res = await api(`/logs?agent_id=${encodeURIComponent(agentId)}`);
      if (!res.ok || agentId !== state.selectedAgentId || requestId <= lastAppliedLogRequest) return;
      lastAppliedLogRequest = requestId;
      const logs = res.logs || [];
      if (state.logsCache && logs.length === state.logsCache.length && logs.every((entry, i) => {
        const old = state.logsCache[i];
        return entry.timestamp_ms === old.timestamp_ms && entry.level === old.level && entry.message === old.message;
      })) return;
      state.logsCache = logs;
      renderLogs();
    } catch (_) {}
  }

  function renderLogs() {
    const tbody = $('logsTbody');
    const filter = $('logLevelFilter').value;
    const rows = (state.logsCache || []).filter((l) => {
      if (filter === 'ERROR') return l.level === 'ERROR';
      if (filter === 'WARN') return l.level === 'ERROR' || l.level === 'WARN';
      return true;
    });
    if (!rows.length) {
      tbody.innerHTML = '<tr><td colspan="3" class="empty-hint">暂无匹配的运行日志</td></tr>';
      return;
    }
    tbody.innerHTML = rows
      .map((l) => {
        const ts = new Date(l.timestamp_ms).toLocaleTimeString();
        const cls = l.level === 'ERROR' ? 'badge-err' : l.level === 'WARN' ? 'badge-warn' : 'badge-info';
        return `
          <tr>
            <td class="mono muted">${ts}</td>
            <td><span class="badge ${cls}">${escapeHtml(l.level)}</span></td>
            <td class="mono" style="white-space:pre-wrap;word-break:break-word">${escapeHtml(l.message)}</td>
          </tr>`;
      })
      .join('');
  }

  // ---------------- Event Wiring ----------------

  function bindRuleTable(tbody, draftKey, fields, deleteClass, render) {
    tbody.addEventListener('change', ({ target }) => {
      const fieldClass = Array.from(target.classList).find((name) => fields[name]);
      if (!fieldClass) return;
      const field = fields[fieldClass];
      let value = target.value;
      if (target.type === 'checkbox') value = target.checked;
      else if (field === 'history_depth') value = parseInt(value, 10) || 10;
      else if (target.type === 'number') value = parseFloat(value) || 0;
      else if (target.type === 'text') value = value.trim();
      if (target.type === 'number' || target.type === 'text') target.value = String(value);
      state[draftKey][target.closest('tr').dataset.idx][field] = value;
      setDirty(true);
    });
    tbody.addEventListener('click', ({ target }) => {
      const button = target.closest(deleteClass);
      if (!button) return;
      state[draftKey].splice(Number(button.closest('tr').dataset.idx), 1);
      setDirty(true);
      render();
    });
  }

  function downloadJson(text, filename) {
    const link = document.createElement('a');
    const url = URL.createObjectURL(new Blob([text], { type: 'application/json' }));
    link.href = url;
    link.download = filename;
    link.click();
    setTimeout(() => URL.revokeObjectURL(url), 0);
  }

  function updateConfigButtons(ag) {
    for (const id of ['btnApplyPresetOne', 'btnPresetImportJson', 'btnPresetExportJson']) $(id).disabled = !ag?.online;
  }

  async function exportConfig() {
    const agent = getSelectedAgent();
    if (!agent?.online) return;
    try {
      const config = await api(`/config/export?agent_id=${encodeURIComponent(agent.agent_id)}`);
      if (config.format_version !== 1) throw new Error(config.message || '配置格式不受支持');
      const timestamp = new Date().toISOString().replace(/[:.]/g, '-');
      const id = agent.agent_id.replace(/[^a-zA-Z0-9_-]/g, '_');
      downloadJson(JSON.stringify(config, null, 2) + '\n', `zenoh-config-${id}-${timestamp}.json`);
    } catch (e) {
      toast(`导出失败：${e.message}`, 'err');
    }
  }

  let pendingImport = null;
  let previewVersion = 0;
  let validatedImport = null;
  let importing = false;
  const sectionInputs = { routes: 'importRoutes', bridge_settings: 'importBridge', session_override: 'importSession' };

  function currentImportOptions() {
    const sections = Object.keys(sectionInputs).filter(key => $(sectionInputs[key]).checked);
    return { config: pendingImport.config, mode: $('configImportMode').value, sections,
      include_agent_id: $('importAgentId').checked };
  }

  function importRequest(agentId, options, dryRun) {
    return api(`/config/import?agent_id=${encodeURIComponent(agentId)}`, {
      method: 'POST', redirect: 'error', body: JSON.stringify({ ...options, dry_run: dryRun }),
    });
  }

  async function previewImport() {
    if (!pendingImport || importing) return;
    const version = ++previewVersion;
    const options = currentImportOptions();
    const target = pendingImport.target;
    const result = $('configImportResult');
    validatedImport = null;
    $('confirmConfigImport').disabled = true;
    result.textContent = '正在校验配置…';
    try {
      const res = await importRequest(target, options, true);
      if (version !== previewVersion) return;
      if (!res.ok || !res.dry_run || !res.summary) throw new Error(res.message || '服务未返回预览');
      const s = res.summary;
      const lines = [`校验通过。路由新增 ${s.added} 条，修改 ${s.modified} 条，删除 ${s.deleted} 条。`,
        `导入后共有出站 ${s.egress} 条、入站 ${s.ingress} 条。`];
      if (s.bridge_settings_changed) lines.push('桥接设置将更新。');
      if (s.session_override_changed) lines.push('Zenoh 会话配置将更新。');
      if (s.agent_id_changed) lines.push(`节点 ID 将改为 ${options.config.agent_id}。`);
      if (state.dirty && options.sections.includes('routes')) lines.push('确认后将重新载入路由，覆盖当前未提交的草稿。');
      result.textContent = lines.join('\n');
      validatedImport = { target, options };
      $('confirmConfigImport').disabled = false;
    } catch (e) {
      if (version === previewVersion) result.textContent = `校验失败：${e.message}`;
    }
  }

  async function confirmImport() {
    if (!validatedImport || importing) return;
    const { target, options } = validatedImport;
    const agent = getSelectedAgent();
    if (!agent?.online || !agent.is_local || target !== state.localAgentId || target !== agent.agent_id) {
      $('configImportResult').textContent = '目标已切换或离线，请重新选择文件。';
      $('confirmConfigImport').disabled = true;
      validatedImport = null;
      return;
    }
    importing = true;
    $('configImportOptions').disabled = true;
    $('confirmConfigImport').disabled = true;
    $('cancelConfigImport').disabled = true;
    try {
      const res = await importRequest(target, options, false);
      if (!res.ok || res.dry_run !== false) throw new Error(res.message || '服务未确认应用');
      $('configDialog').close();
      if (options.sections.includes('routes')) setDirty(false);
      state.sessionLoadedForAgent = '';
      await pollAgents();
      toast(`${res.agent_id || target} 配置已导入。`);
    } catch (e) {
      $('configImportResult').textContent = `导入未获确认：${e.message}。请重新选择文件预览。`;
    } finally {
      importing = false;
      validatedImport = null;
      $('cancelConfigImport').disabled = false;
      $('configImportOptions').disabled = false;
    }
  }

  function initEvents() {
    bindRuleTable($('egressTbody'), 'draftEgress', EGRESS_FIELDS, '.eg-del', renderEgressTable);
    bindRuleTable($('ingressTbody'), 'draftIngress', INGRESS_FIELDS, '.ig-del', renderIngressTable);
    $('mainTabs').querySelectorAll('.tab').forEach((btn) => {
      btn.addEventListener('click', () => {
        const target = btn.getAttribute('data-tab');
        state.activeTab = target;
        $('mainTabs').querySelectorAll('.tab').forEach((b) => b.classList.remove('active'));
        btn.classList.add('active');
        document.querySelectorAll('.tab-panel').forEach((p) => p.classList.remove('active'));
        $(`tab-${target}`).classList.add('active');
        if (target === 'session') loadZenohInfo(false);
        if (target === 'logs') pollLogs();
        if (target === 'discovery') renderDiscovery();
      });
    });

    $('logLevelFilter').addEventListener('change', renderLogs);
    $('btnRefreshLogs').addEventListener('click', pollLogs);
    $('btnClearLogs').addEventListener('click', async () => {
      const agentId = state.selectedAgentId;
      if (!agentId) return;
      const res = await api('/logs/clear', {
        method: 'POST',
        body: JSON.stringify({ agent_id: agentId }),
      });
      if (res.ok) {
        if (state.selectedAgentId === agentId) {
          lastAppliedLogRequest = logRequestId;
          state.logsCache = [];
          renderLogs();
        }
        toast(`已清空 ${agentId} 的日志`, 'ok');
        pollAgents();
      }
    });

    $('btnRefreshFleet').addEventListener('click', pollAgents);
    $('btnAddEgress').addEventListener('click', () => {
      state.draftEgress.push({
        topic: '',
        type: '',
        zenoh_key: '',
        max_freq: 30,
        max_kbps: 0,
        enabled: true,
        on_demand: false,
        priority: 'data_high',
        congestion: 'drop',
        express: true,
        reliability: 'reliable',
        locality: 'any',
        qos: 'auto',
        history_depth: 10,
      });
      setDirty(true);
      renderEgressTable();
    });

    $('btnAddIngress').addEventListener('click', () => {
      state.draftIngress.push({
        pattern: '*/topic_name',
        remap_prefix: '',
        exclude_self: true,
        max_freq: 30,
        enabled: true,
        locality: 'any',
        qos: 'auto',
        history_depth: 10,
      });
      setDirty(true);
      renderIngressTable();
    });

    $('configImportOptions').addEventListener('change', previewImport);
    $('confirmConfigImport').addEventListener('click', confirmImport);
    $('cancelConfigImport').addEventListener('click', () => $('configDialog').close());
    $('configDialog').addEventListener('cancel', e => { if (importing) e.preventDefault(); });
    $('configDialog').addEventListener('close', () => {
      ++previewVersion;
      pendingImport = validatedImport = null;
    });

    const CUSTOM_PRESETS_KEY = 'zenoh_custom_presets_v1';
    function loadCustomPresets() {
      try {
        const raw = JSON.parse(localStorage.getItem(CUSTOM_PRESETS_KEY) || '[]');
        return Array.isArray(raw) ? raw.filter(p => p && typeof p.id === 'string' && typeof p.name === 'string') : [];
      } catch {
        return [];
      }
    }
    function saveCustomPresets(list) {
      localStorage.setItem(CUSTOM_PRESETS_KEY, JSON.stringify(list));
    }
    function renderCustomPresets(selectValue) {
      const group = $('customPresetGroup');
      if (!group) return;
      const list = loadCustomPresets();
      group.innerHTML = '';
      for (const item of list) {
        const opt = document.createElement('option');
        opt.value = `custom:${item.id}`;
        const eg = item.routes?.egress?.length || 0;
        const ig = item.routes?.ingress?.length || 0;
        opt.textContent = `${item.name} (出${eg}/入${ig})`;
        group.appendChild(opt);
      }
      if (selectValue) $('presetSelect').value = selectValue;
    }
    function extractRoutesFromJson(config) {
      const routes = config?.routes ?? config;
      if (!routes || !('egress' in routes || 'ingress' in routes)) return null;
      for (const key of ['egress', 'ingress']) {
        if (key in routes && !Array.isArray(routes[key])) throw new Error(`routes.${key} 必须为数组`);
      }
      return { egress: routes.egress || [], ingress: routes.ingress || [] };
    }

    async function applyRoutePreset(agentId, routes, save) {
      const preview = await importRequest(state.localAgentId, {
        config: { format_version: 1, agent_id: agentId, routes },
        sections: ['routes'], include_agent_id: true, mode: 'replace',
      }, true);
      if (!preview.ok || !preview.dry_run) throw new Error(preview.message || '路由校验失败');
      if (state.selectedAgentId !== agentId || !getSelectedAgent()?.online) throw new Error('目标节点已切换或离线');
      const res = await api('/rules', {
        method: 'POST',
        body: JSON.stringify({ agent_id: agentId, ...routes, save }),
      });
      if (!res.ok) throw new Error(res.message || '路由更新失败');
      if (state.selectedAgentId === agentId) setDirty(false);
      await pollAgents();
      toast(`${agentId} 路由已${save ? '应用并保存' : '临时应用'}`, 'ok');
    }

    $('btnApplyLive')?.addEventListener('click', () => submitRules(false));
    $('btnApplySave')?.addEventListener('click', () => submitRules(true));

    renderCustomPresets();
    $('btnPresetExportJson').addEventListener('click', exportConfig);
    $('btnPresetImportJson').addEventListener('click', () => $('presetJsonFileInput').click());
    $('presetJsonFileInput')?.addEventListener('change', async (e) => {
      const file = e.target.files?.[0];
      e.target.value = '';
      const agent = getSelectedAgent();
      if (!file || !agent?.online) return;
      const save = $('chkPresetSave').checked;
      try {
        const parsed = JSON.parse(await file.text());
        if (parsed?.format_version != null && parsed.format_version !== 1) throw new Error('配置格式版本不受支持');
        if (state.selectedAgentId !== agent.agent_id || !getSelectedAgent()?.online) throw new Error('目标节点已切换或离线');
        const routes = extractRoutesFromJson(parsed);
        if (!routes) throw new Error('JSON 中未找到 routes.egress / routes.ingress 路由规则');
        const baseName = file.name.replace(/\.json$/i, '');
        const id = `${baseName.replace(/[^a-zA-Z0-9_-]/g, '_')}_${Date.now().toString(36)}`;
        const list = loadCustomPresets().filter(p => p.name !== baseName);
        list.unshift({ id, name: baseName, routes, raw: parsed });
        saveCustomPresets(list.slice(0, 30));
        renderCustomPresets(`custom:${id}`);

        const fullConfig = parsed.format_version === 1 && (parsed.bridge_settings || parsed.session_override);
        if (!fullConfig) {
          await applyRoutePreset(agent.agent_id, routes, save);
        } else if (agent.is_local && agent.agent_id === state.localAgentId) {
          pendingImport = { config: parsed, target: agent.agent_id };
          $('configImportMode').value = 'merge';
          for (const [section, sid] of Object.entries(sectionInputs)) {
            $(sid).disabled = !parsed[section];
            $(sid).checked = Boolean(parsed[section]);
          }
          $('importAgentId').checked = false;
          $('configImportSource').textContent = `外部预设 ${file.name} → 本机 ${agent.agent_id}（已同时保存到左下角预设列表）`;
          $('configDialog').showModal();
          await previewImport();
        } else {
          toast(`已载入外部预设「${baseName}」（出${routes.egress.length}/入${routes.ingress.length}），点击「应用当前节点」即可生效`, 'ok');
        }
      } catch (err) {
        toast(`导入外部 JSON 失败：${err.message}`, 'err');
      }
    });

    $('btnApplyPresetOne').addEventListener('click', async () => {
      const preset = $('presetSelect').value;
      const save = $('chkPresetSave').checked;
      if (!getSelectedAgent()?.online) return;
      if (preset === 'current') {
        await submitRules(save);
        return;
      }
      if (preset.startsWith('custom:')) {
        const item = loadCustomPresets().find(p => p.id === preset.slice('custom:'.length));
        try {
          if (!item) throw new Error('未找到该自定义预设');
          await applyRoutePreset(state.selectedAgentId, item.routes, save);
        } catch (e) {
          toast(e.message, 'err');
        }
        return;
      }
      const res = await api('/preset', {
        method: 'POST',
        body: JSON.stringify({ agent_id: state.selectedAgentId, preset, save }),
      });
      if (res.ok) {
        toast(`已将预设 ${preset} 应用到 ${state.selectedAgentId}`, 'ok');
        setDirty(false);
        await pollAgents();
      } else {
        toast(res.message || '预设应用失败', 'err');
      }
    });

    $('topicSearchInput').addEventListener('input', renderDiscovery);
    $('btnScanTopics').addEventListener('click', async () => {
      if (!state.selectedAgentId) return;
      await api(`/rules?agent_id=${encodeURIComponent(state.selectedAgentId)}`);
      await pollAgents();
      toast('话题列表已刷新', 'ok');
    });

    // Session & Bridge Settings events
    $('btnDdsShieldPill').addEventListener('click', toggleDdsShieldForSelected);
    $('btnCoreShieldApply').addEventListener('click', applyCoreShieldBar);
    $('btnRefreshInfo').addEventListener('click', () => loadZenohInfo(true));
    $('btnSaveBridgeSettings').addEventListener('click', saveBridgeSettings);
    $('btnModeStructured').addEventListener('click', () => setConfigMode('structured'));
    $('btnModeRawJson5').addEventListener('click', () => {
      setConfigMode('raw_json5');
      if (!$('sessRawJson5').value.trim() && state.lastEffectiveConfigText) {
        $('sessRawJson5').value = state.lastEffectiveConfigText;
      }
    });
    $('btnSwitchToRawWithEffective').addEventListener('click', () => {
      setConfigMode('raw_json5');
      if (state.lastEffectiveConfigText) {
        $('sessRawJson5').value = state.lastEffectiveConfigText;
      }
      toast('已载入当前生效完整配置到原生 JSON5 编辑器', 'ok');
    });
    $('btnRawLoadEffective').addEventListener('click', () => {
      if (state.lastEffectiveConfigText) {
        $('sessRawJson5').value = state.lastEffectiveConfigText;
        toast('已载入当前生效配置', 'ok');
      }
    });
    $('btnRawLoadDefault').addEventListener('click', async () => {
      const res = await api('/zenoh/default_config');
      if (res.ok && res.default_config) {
        $('sessRawJson5').value = JSON.stringify(res.default_config, null, 2);
        toast('已载入 Zenoh 1.8.0 官方默认模板', 'ok');
      } else {
        toast(res.message || '载入默认模板失败', 'err');
      }
    });
    $('btnRawImport').addEventListener('click', () => $('rawJson5FileInput').click());
    $('rawJson5FileInput').addEventListener('change', async (e) => {
      const file = e.target.files?.[0];
      if (!file) return;
      $('sessRawJson5').value = await file.text();
      toast(`已导入 ${file.name}`, 'ok');
      e.target.value = '';
    });
    $('btnRawExport').addEventListener('click', () => {
      const txt = $('sessRawJson5').value || state.lastEffectiveConfigText || '{}';
      downloadJson(txt, `zenoh-config-${state.selectedAgentId || 'agent'}.json5`);
    });
    $('btnCopyEffective').addEventListener('click', async () => {
      try {
        await navigator.clipboard.writeText(state.lastEffectiveConfigText || '');
        toast('已复制当前生效 JSON5 配置', 'ok');
      } catch (_) {
        toast('复制失败，请手动选中复制', 'err');
      }
    });
    $('btnValidateConfig').addEventListener('click', validateSessionConfig);
    $('btnReconnectSession').addEventListener('click', reconnectSession);

    // Zenoh Lab events
    $('btnLabPut').addEventListener('click', async () => {
      const res = await api('/zenoh/put', {
        method: 'POST',
        body: JSON.stringify({
          key_expr: $('labPutKey').value.trim(),
          encoding: $('labPutEnc').value,
          priority: $('labPutPrio').value,
          congestion: $('labPutCong').value,
          reliability: $('labPutRel').value,
          locality: $('labPutLoc').value,
          express: $('labPutExpress').checked,
          attachment: $('labPutAtt').value.trim(),
          payload: $('labPutPayload').value,
        }),
      });
      $('labPutResult').textContent = JSON.stringify(res, null, 2);
      toast(res.ok ? 'PUT 发送成功' : (res.message || 'PUT 失败'), res.ok ? 'ok' : 'err');
    });

    $('btnLabDelete').addEventListener('click', async () => {
      const res = await api('/zenoh/delete', {
        method: 'POST',
        body: JSON.stringify({
          key_expr: $('labPutKey').value.trim(),
          priority: $('labPutPrio').value,
          congestion: $('labPutCong').value,
          reliability: $('labPutRel').value,
          express: $('labPutExpress').checked,
        }),
      });
      $('labPutResult').textContent = JSON.stringify(res, null, 2);
      toast(res.ok ? 'DELETE 发送成功' : (res.message || 'DELETE 失败'), res.ok ? 'ok' : 'err');
    });

    document.querySelectorAll('.chip-btn[data-get-key]').forEach((chip) => {
      chip.addEventListener('click', () => {
        $('labGetKey').value = chip.getAttribute('data-get-key') || '';
        $('labGetPayload').value = chip.getAttribute('data-get-payload') || '';
      });
    });

    $('btnLabGet').addEventListener('click', async () => {
      $('labGetResult').textContent = '查询中...';
      const res = await api('/zenoh/get', {
        method: 'POST',
        body: JSON.stringify({
          key_expr: $('labGetKey').value.trim(),
          target: $('labGetTarget').value,
          consolidation: $('labGetCons').value,
          timeout_ms: parseInt($('labGetTimeout').value, 10) || 3000,
          payload: $('labGetPayload').value,
        }),
      });
      $('labGetResult').textContent = JSON.stringify(res, null, 2);
    });

    $('btnScoutRun').addEventListener('click', runScout);
    $('btnLiveQuery').addEventListener('click', queryLiveliness);
    $('btnLiveDeclare').addEventListener('click', async () => {
      const key = $('liveDeclareKey').value.trim();
      if (!key) return toast('请输入要声明的 Liveliness Key', 'err');
      const res = await api('/zenoh/liveliness/declare', {
        method: 'POST',
        body: JSON.stringify({ key_expr: key }),
      });
      toast(res.message || (res.ok ? '已声明' : '声明失败'), res.ok ? 'ok' : 'err');
      if (res.ok) queryLiveliness();
    });
    $('btnLiveUndeclare').addEventListener('click', async () => {
      const key = $('liveDeclareKey').value.trim();
      if (!key) return toast('请输入要注销的 Liveliness Key', 'err');
      const res = await api('/zenoh/liveliness/undeclare', {
        method: 'POST',
        body: JSON.stringify({ key_expr: key }),
      });
      toast(res.message || (res.ok ? '已注销' : '注销失败'), res.ok ? 'ok' : 'err');
      if (res.ok) queryLiveliness();
    });

    $('btnSniffStart').addEventListener('click', async () => {
      const res = await api('/zenoh/sniff/start', {
        method: 'POST',
        body: JSON.stringify({ key_expr: $('sniffKeyInput').value.trim() }),
      });
      if (res.ok) {
        state.sniffActive = true;
        toast(`开始监听 ${res.key_expr}`, 'ok');
        pollSniffer();
      } else {
        toast(res.message || '启动监听失败', 'err');
      }
    });

    $('btnSniffStop').addEventListener('click', async () => {
      await api('/zenoh/sniff/stop', { method: 'POST' });
      state.sniffActive = false;
      toast('已停止监听', 'ok');
    });

    $('btnSniffClear').addEventListener('click', async () => {
      await api('/zenoh/sniff/clear', { method: 'POST' });
      $('sniffTbody').innerHTML = '';
    });
  }

  initEvents();
  pollAgents();
  setInterval(() => { if (!currentPoll) pollAgents(); }, 1500);
  setInterval(pollSniffer, 1000);
})();
