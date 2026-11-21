(() => {
  const API_BASE = new URL('../api', window.location.href).href.replace(/\/$/, '');

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
    logsCache: [],
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
      headers: { 'Content-Type': 'application/json', ...(options.headers || {}) },
      ...options,
    });
    const data = await resp.json().catch(() => ({ ok: false, message: `HTTP ${resp.status}` }));
    if (!resp.ok && data.ok === undefined) {
      data.ok = false;
    }
    return data;
  }

  function setDirty(flag) {
    state.dirty = flag;
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

  async function pollAgents() {
    try {
      const data = await api('/agents');
      if (!data.ok) throw new Error(data.message || 'API error');

      $('apiStatusDot').className = 'conn-dot ok';
      state.localAgentId = data.local_agent_id;
      state.agents = data.agents || [];

      if (!state.selectedAgentId || !state.agents.some((a) => a.agent_id === state.selectedAgentId)) {
        state.selectedAgentId = state.localAgentId || (state.agents[0]?.agent_id ?? '');
        loadDraftFromSelectedAgent();
        state.sessionLoadedForAgent = '';
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

      if (!state.dirty) {
        loadDraftFromSelectedAgent();
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
      $('localSummary').textContent = `无法连接后端 API: ${e.message}`;
    }
  }

  function loadDraftFromSelectedAgent() {
    const ag = getSelectedAgent();
    if (!ag) return;
    state.draftEgress = JSON.parse(JSON.stringify(ag.egress || []));
    state.draftIngress = JSON.parse(JSON.stringify(ag.ingress || []));
    setDirty(false);
    renderEgressTable();
    renderIngressTable();
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
        const errTag = (ag.error_count || 0) > 0
          ? `<span class="badge badge-err" title="存在 ${ag.error_count} 条错误日志">! ${ag.error_count}</span>`
          : '';
        return `
          <div class="fleet-card ${active}" data-agent="${escapeHtml(ag.agent_id)}">
            <div class="fleet-row">
              <div class="fleet-name"><span class="dot ${dot}"></span>${escapeHtml(ag.agent_id)}</div>
              <div class="btn-row" style="gap:4px">${errTag}${localTag}</div>
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
          state.selectedAgentId = id;
          state.sessionLoadedForAgent = '';
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
          ? `<div class="muted" style="font-size:10px">${escapeHtml(r.resolved_type)}</div>`
          : '<div class="muted" style="font-size:10px">等待本机发布...</div>';
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

    tbody.querySelectorAll('tr').forEach((tr) => {
      const idx = Number(tr.getAttribute('data-idx'));
      const bind = (sel, field, fn) => {
        const el = tr.querySelector(sel);
        if (!el) return;
        el.addEventListener('change', () => {
          state.draftEgress[idx][field] = fn(el);
          setDirty(true);
        });
      };
      bind('.eg-enabled', 'enabled', (el) => el.checked);
      bind('.eg-topic', 'topic', (el) => el.value.trim());
      bind('.eg-zkey', 'zenoh_key', (el) => el.value.trim());
      bind('.eg-freq', 'max_freq', (el) => parseFloat(el.value) || 0);
      bind('.eg-kbps', 'max_kbps', (el) => parseFloat(el.value) || 0);
      bind('.eg-ondemand', 'on_demand', (el) => el.checked);
      bind('.eg-prio', 'priority', (el) => el.value);
      bind('.eg-cong', 'congestion', (el) => el.value);
      bind('.eg-express', 'express', (el) => el.checked);
      bind('.eg-rel', 'reliability', (el) => el.value);
      bind('.eg-loc', 'locality', (el) => el.value);
      bind('.eg-qos', 'qos', (el) => el.value);
      bind('.eg-depth', 'history_depth', (el) => parseInt(el.value, 10) || 10);

      tr.querySelector('.eg-del')?.addEventListener('click', () => {
        state.draftEgress.splice(idx, 1);
        setDirty(true);
        renderEgressTable();
      });
    });
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

    tbody.querySelectorAll('tr').forEach((tr) => {
      const idx = Number(tr.getAttribute('data-idx'));
      const bind = (sel, field, fn) => {
        const el = tr.querySelector(sel);
        if (!el) return;
        el.addEventListener('change', () => {
          state.draftIngress[idx][field] = fn(el);
          setDirty(true);
        });
      };
      bind('.ig-enabled', 'enabled', (el) => el.checked);
      bind('.ig-pat', 'pattern', (el) => el.value.trim());
      bind('.ig-remap', 'remap_prefix', (el) => el.value.trim());
      bind('.ig-excl', 'exclude_self', (el) => el.checked);
      bind('.ig-freq', 'max_freq', (el) => parseFloat(el.value) || 0);
      bind('.ig-loc', 'locality', (el) => el.value);
      bind('.ig-qos', 'qos', (el) => el.value);
      bind('.ig-depth', 'history_depth', (el) => parseInt(el.value, 10) || 10);

      tr.querySelector('.ig-del')?.addEventListener('click', () => {
        state.draftIngress.splice(idx, 1);
        setDirty(true);
        renderIngressTable();
      });
    });
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
        toast(`已将 ${name} 加入出站草稿，点击「热更新」即可生效`, 'ok');
      });
    });
  }

  // ---------------- Zenoh Kernel & Bridge Settings ----------------

  function populateSessionForm(c) {
    setConfigMode(c.config_mode || 'structured');
    $('sessRawJson5').value = c.raw_json5 || '';

    // Group 1
    $('sessMode').value = c.mode || 'peer';
    $('sessCustomZid').value = c.custom_zid || '';
    $('sessNamespace').value = c.zenoh_namespace || '';
    $('sessQueriesTimeout').value = c.queries_default_timeout_ms ?? 10000;
    $('sessConnect').value = (c.connect_endpoints || []).join('\n');
    $('sessConnectTimeout').value = c.connect_timeout_ms ?? 0;
    $('sessConnectExitFail').checked = Boolean(c.connect_exit_on_failure);
    $('sessListen').value = (c.listen_endpoints || []).join('\n');
    $('sessListenTimeout').value = c.listen_timeout_ms ?? 0;
    $('sessListenExitFail').checked = Boolean(c.listen_exit_on_failure);
    $('sessMetadataJson5').value = c.metadata_json5 || '';

    // Group 2
    $('sessScoutTimeout').value = c.scouting_timeout_ms ?? 3000;
    $('sessScoutDelay').value = c.scouting_delay_ms ?? 500;
    $('sessMcastAddr').value = c.multicast_address || '224.0.0.224:7446';
    $('sessMcastIface').value = c.multicast_interface || 'auto';
    $('sessMcastTtl').value = c.multicast_ttl ?? 1;
    $('sessMcastEn').checked = c.multicast_enabled !== false;
    $('sessMcastListen').checked = c.multicast_listen !== false;
    $('sessGossipEn').checked = c.gossip_enabled !== false;
    $('sessGossipMulti').checked = Boolean(c.gossip_multihop);

    // Group 3
    $('sessLowLatency').checked = Boolean(c.unicast_lowlatency);
    $('sessUnicastQos').checked = c.unicast_qos_enabled !== false;
    $('sessCompression').checked = Boolean(c.compression_enabled);
    $('sessBatching').checked = c.batching_enabled !== false;
    $('sessMcastQos').checked = Boolean(c.multicast_qos_enabled);
    $('sessMcastComp').checked = Boolean(c.multicast_compression_enabled);
    $('sessMaxSessions').value = c.unicast_max_sessions ?? 1000;
    $('sessMaxLinks').value = c.unicast_max_links ?? 1;
    $('sessOpenTimeout').value = c.unicast_open_timeout ?? 10000;
    $('sessAcceptTimeout').value = c.unicast_accept_timeout ?? 10000;
    $('sessLeaseMs').value = c.lease_ms ?? 10000;
    $('sessKeepAlive').value = c.keep_alive ?? 4;
    $('sessTxBatchSize').value = c.tx_batch_size ?? 65535;
    $('sessBatchTimeLimit').value = c.batching_time_limit_ms ?? 1;
    $('sessTxThreads').value = c.tx_threads ?? 0;
    $('sessMcastJoinInt').value = c.multicast_join_interval ?? 2500;
    $('sessRxBufSize').value = c.rx_buffer_size ?? 65535;
    $('sessRxMaxMsg').value = c.rx_max_message_size ?? 1073741824;
    $('sessTcpSndBuf').value = c.tcp_so_sndbuf ?? 0;
    $('sessTcpRcvBuf').value = c.tcp_so_rcvbuf ?? 0;
    $('sessTxQueueJson5').value = c.tx_queue_json5 || '';

    // Group 4: SHM
    $('sessShm').checked = Boolean(c.shared_memory_enabled);
    $('sessShmOpt').checked = c.shm_opt_enabled !== false;
    $('sessShmMode').value = c.shm_mode || 'lazy';
    $('sessShmPoolSize').value = c.shm_pool_size ?? 16777216;
    $('sessShmThreshold').value = c.shm_message_threshold ?? 3072;

    // Group 5: Security
    $('sessTlsRootCa').value = c.tls_root_ca || '';
    $('sessTlsListenCert').value = c.tls_listen_cert || '';
    $('sessTlsListenKey').value = c.tls_listen_key || '';
    $('sessTlsConnectCert').value = c.tls_connect_cert || '';
    $('sessTlsConnectKey').value = c.tls_connect_key || '';
    $('sessTlsMtls').checked = Boolean(c.tls_enable_mtls);
    $('sessTlsVerifyName').checked = c.tls_verify_name !== false;
    $('sessAuthUser').value = c.auth_user || '';
    $('sessAuthPass').value = c.auth_password || '';
    $('sessAuthDict').value = c.auth_dictionary_file || '';
    $('sessAuthPubKey').value = c.auth_pubkey_file || '';
    $('sessAuthPrivKey').value = c.auth_privkey_file || '';
    $('sessAuthKnownKeys').value = c.auth_known_keys_file || '';
    $('sessAclEnabled').checked = Boolean(c.acl_enabled);
    $('sessAclDefaultPerm').value = c.acl_default_permission || 'deny';
    $('sessAclJson5').value = c.acl_json5 || '';

    // Group 6: Routing, AdminSpace, Flow Control & Plugins
    $('sessTimestamp').checked = Boolean(c.timestamping_enabled);
    $('sessDropFutureTs').checked = Boolean(c.drop_future_timestamp);
    $('sessFailoverBroker').checked = c.peers_failover_brokering !== false;
    $('sessAdminEn').checked = Boolean(c.adminspace_enabled);
    $('sessAdminRead').checked = c.adminspace_read !== false;
    $('sessAdminWrite').checked = Boolean(c.adminspace_write);
    $('sessPluginsLoad').checked = Boolean(c.plugins_loading_enabled);
    $('sessPeerRoutingMode').value = c.peer_routing_mode || 'peer_to_peer';
    $('sessDownsampling').value = c.downsampling_json5 || '';
    $('sessLowPass').value = c.low_pass_filter_json5 || '';
    $('sessQosPub').value = c.qos_publication_json5 || '';
    $('sessQosNet').value = c.qos_network_json5 || '';
    $('sessAggregation').value = c.aggregation_json5 || '';
    $('sessPluginsJson5').value = c.plugins_json5 || '';

    // Group 7: True JSON5 Override
    $('sessCustomJson').value = c.custom_json5 || '';
  }

  function collectSessionConfigFromForm() {
    const parseLines = (txt) => txt.split(/\r?\n/).map((s) => s.trim()).filter(Boolean);
    const numVal = (id, fallback) => {
      const v = parseInt($(id).value, 10);
      return Number.isFinite(v) ? v : fallback;
    };
    return {
      config_mode: state.configMode,
      raw_json5: $('sessRawJson5').value,

      custom_zid: $('sessCustomZid').value.trim(),
      zenoh_namespace: $('sessNamespace').value.trim(),
      metadata_json5: $('sessMetadataJson5').value.trim(),
      mode: $('sessMode').value,
      connect_endpoints: parseLines($('sessConnect').value),
      connect_timeout_ms: numVal('sessConnectTimeout', 0),
      connect_exit_on_failure: $('sessConnectExitFail').checked,
      listen_endpoints: parseLines($('sessListen').value),
      listen_timeout_ms: numVal('sessListenTimeout', 0),
      listen_exit_on_failure: $('sessListenExitFail').checked,
      queries_default_timeout_ms: numVal('sessQueriesTimeout', 10000),

      scouting_timeout_ms: numVal('sessScoutTimeout', 3000),
      scouting_delay_ms: numVal('sessScoutDelay', 500),
      multicast_enabled: $('sessMcastEn').checked,
      multicast_address: $('sessMcastAddr').value.trim(),
      multicast_interface: $('sessMcastIface').value.trim() || 'auto',
      multicast_ttl: numVal('sessMcastTtl', 1),
      multicast_listen: $('sessMcastListen').checked,
      gossip_enabled: $('sessGossipEn').checked,
      gossip_multihop: $('sessGossipMulti').checked,

      unicast_lowlatency: $('sessLowLatency').checked,
      unicast_qos_enabled: $('sessUnicastQos').checked,
      compression_enabled: $('sessCompression').checked,
      unicast_max_sessions: numVal('sessMaxSessions', 1000),
      unicast_max_links: numVal('sessMaxLinks', 1),
      unicast_open_timeout: numVal('sessOpenTimeout', 10000),
      unicast_accept_timeout: numVal('sessAcceptTimeout', 10000),
      multicast_qos_enabled: $('sessMcastQos').checked,
      multicast_compression_enabled: $('sessMcastComp').checked,
      multicast_join_interval: numVal('sessMcastJoinInt', 2500),
      lease_ms: numVal('sessLeaseMs', 10000),
      keep_alive: numVal('sessKeepAlive', 4),
      tx_batch_size: numVal('sessTxBatchSize', 65535),
      tx_threads: numVal('sessTxThreads', 0),
      batching_enabled: $('sessBatching').checked,
      batching_time_limit_ms: numVal('sessBatchTimeLimit', 1),
      rx_buffer_size: numVal('sessRxBufSize', 65535),
      rx_max_message_size: numVal('sessRxMaxMsg', 1073741824),
      tcp_so_sndbuf: numVal('sessTcpSndBuf', 0),
      tcp_so_rcvbuf: numVal('sessTcpRcvBuf', 0),
      tx_queue_json5: $('sessTxQueueJson5').value.trim(),

      shared_memory_enabled: $('sessShm').checked,
      shm_mode: $('sessShmMode').value,
      shm_opt_enabled: $('sessShmOpt').checked,
      shm_pool_size: numVal('sessShmPoolSize', 16777216),
      shm_message_threshold: numVal('sessShmThreshold', 3072),

      timestamping_enabled: $('sessTimestamp').checked,
      drop_future_timestamp: $('sessDropFutureTs').checked,

      tls_root_ca: $('sessTlsRootCa').value.trim(),
      tls_listen_key: $('sessTlsListenKey').value.trim(),
      tls_listen_cert: $('sessTlsListenCert').value.trim(),
      tls_enable_mtls: $('sessTlsMtls').checked,
      tls_connect_key: $('sessTlsConnectKey').value.trim(),
      tls_connect_cert: $('sessTlsConnectCert').value.trim(),
      tls_verify_name: $('sessTlsVerifyName').checked,
      auth_user: $('sessAuthUser').value.trim(),
      auth_password: $('sessAuthPass').value,
      auth_dictionary_file: $('sessAuthDict').value.trim(),
      auth_pubkey_file: $('sessAuthPubKey').value.trim(),
      auth_privkey_file: $('sessAuthPrivKey').value.trim(),
      auth_known_keys_file: $('sessAuthKnownKeys').value.trim(),
      acl_enabled: $('sessAclEnabled').checked,
      acl_default_permission: $('sessAclDefaultPerm').value,
      acl_json5: $('sessAclJson5').value.trim(),

      peers_failover_brokering: $('sessFailoverBroker').checked,
      peer_routing_mode: $('sessPeerRoutingMode').value,
      adminspace_enabled: $('sessAdminEn').checked,
      adminspace_read: $('sessAdminRead').checked,
      adminspace_write: $('sessAdminWrite').checked,
      downsampling_json5: $('sessDownsampling').value.trim(),
      low_pass_filter_json5: $('sessLowPass').value.trim(),
      qos_publication_json5: $('sessQosPub').value.trim(),
      qos_network_json5: $('sessQosNet').value.trim(),
      aggregation_json5: $('sessAggregation').value.trim(),
      plugins_loading_enabled: $('sessPluginsLoad').checked,
      plugins_json5: $('sessPluginsJson5').value.trim(),

      custom_json5: $('sessCustomJson').value.trim(),
    };
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

  async function loadZenohInfo(forceFormReload = false) {
    const target = state.selectedAgentId || state.localAgentId;
    const qs = target ? `?agent_id=${encodeURIComponent(target)}` : '';
    const res = await api(`/zenoh/info${qs}`);
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

    if (forceFormReload || state.sessionLoadedForAgent !== target) {
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
      state.sessionLoadedForAgent = '';
      if (target === state.localAgentId && res.agent_id) {
        state.selectedAgentId = res.agent_id;
      }
      setTimeout(async () => {
        await pollAgents();
        await loadZenohInfo(true);
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
      state.sessionLoadedForAgent = '';
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
        const cur = $('sessConnect').value.split(/\r?\n/).map((s) => s.trim()).filter(Boolean);
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
    if (!state.selectedAgentId) return;
    try {
      const res = await api(`/logs?agent_id=${encodeURIComponent(state.selectedAgentId)}`);
      if (!res.ok) return;
      state.logsCache = res.logs || [];
      renderLogs();
    } catch (_) {}
  }

  function renderLogs() {
    const tbody = $('logsTbody');
    const filter = $('logLevelFilter').value;
    const rows = state.logsCache.filter((l) => {
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

  function initEvents() {
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
      });
    });

    $('logLevelFilter').addEventListener('change', renderLogs);
    $('btnRefreshLogs').addEventListener('click', pollLogs);
    $('btnClearLogs').addEventListener('click', async () => {
      if (!state.selectedAgentId) return;
      const res = await api('/logs/clear', {
        method: 'POST',
        body: JSON.stringify({ agent_id: state.selectedAgentId }),
      });
      if (res.ok) {
        state.logsCache = [];
        renderLogs();
        toast(`已清空 ${state.selectedAgentId} 的日志`, 'ok');
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

    $('btnReloadRules').addEventListener('click', () => {
      loadDraftFromSelectedAgent();
      toast('已重新载入当前生效规则', 'ok');
    });
    $('btnApplyLive').addEventListener('click', () => submitRules(false));
    $('btnApplySave').addEventListener('click', () => submitRules(true));

    $('btnExportRules').addEventListener('click', () => {
      const blob = new Blob(
        [JSON.stringify({ egress: state.draftEgress, ingress: state.draftIngress }, null, 2)],
        { type: 'application/json' }
      );
      const a = document.createElement('a');
      a.href = URL.createObjectURL(blob);
      a.download = `zenoh-rules-${state.selectedAgentId || 'agent'}.json`;
      a.click();
    });

    $('btnImportRules').addEventListener('click', () => $('importFileInput').click());
    $('importFileInput').addEventListener('change', async (e) => {
      const file = e.target.files?.[0];
      if (!file) return;
      try {
        const j = JSON.parse(await file.text());
        if (Array.isArray(j.egress)) state.draftEgress = j.egress;
        if (Array.isArray(j.ingress)) state.draftIngress = j.ingress;
        setDirty(true);
        renderEgressTable();
        renderIngressTable();
        toast('已导入规则草稿，请点击热更新使其生效', 'ok');
      } catch (err) {
        toast(`导入失败: ${err.message}`, 'err');
      }
      e.target.value = '';
    });

    $('btnApplyPresetOne').addEventListener('click', async () => {
      const preset = $('presetSelect').value;
      const save = $('chkPresetSave').checked;
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

    $('btnApplyPresetAll').addEventListener('click', async () => {
      const preset = $('presetSelect').value;
      const save = $('chkPresetSave').checked;
      const res = await api('/preset', {
        method: 'POST',
        body: JSON.stringify({ agent_id: 'all', preset, save }),
      });
      if (res.ok) {
        toast(`已将预设 ${preset} 批量推送到全部在线节点`, 'ok');
        setDirty(false);
        await pollAgents();
      } else {
        toast(res.message || '批量推送失败', 'err');
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
      const blob = new Blob([txt], { type: 'application/json' });
      const a = document.createElement('a');
      a.href = URL.createObjectURL(blob);
      a.download = `zenoh-config-${state.selectedAgentId || 'agent'}.json5`;
      a.click();
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
  setInterval(pollAgents, 1500);
  setInterval(pollSniffer, 1000);
})();
