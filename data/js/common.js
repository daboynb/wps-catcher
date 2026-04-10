const WS_URL = `ws://${location.hostname || '192.168.4.1'}/ws`;
const CMD = {
    GET_STATUS: 0, SET_AP_SETTINGS: 1, GET_AP_SETTINGS: 2, REBOOT: 3,
    SET_STA_SETTINGS: 4, GET_STA_SETTINGS: 5,
    WPS_START: 6, WPS_STOP: 7, WPS_STATUS: 8,
    SAVED_WIFI_LIST: 9, SAVED_WIFI_DELETE: 10,
    SET_WEB_PASS: 11
};

let ws = null, reqId = 0, pending = new Map();
let connected = false, statusTimer = null, wsAuthFails = 0;

/* Page-specific hooks (set by page JS) */
let onWpsPush = null;   /* WPS push (msg.type ws/wr/we) */
let onWsOpen = null;    /* Called when WS connects */
let onWsClose = null;   /* Called when WS disconnects */

/* ---- SIDEBAR ---- */
function initSidebar(activePage) {
    const sections = [
        { label: '', items: [
            { id: 'wps', icon: '&#9888;', text: 'WPS Listener', href: '/wps.html' }
        ]},
        { label: 'System', items: [
            { id: 'config', icon: '&#9881;', text: 'Config', href: '/config.html' }
        ]}
    ];

    let nav = '';
    sections.forEach(s => {
        if (s.label) nav += `<div class="sb-label">${s.label}</div>`;
        s.items.forEach(it => {
            const cls = it.id === activePage ? 'sb-item active' : 'sb-item';
            nav += `<a class="${cls}" href="${it.href}"><i class="sb-icon">${it.icon}</i>${it.text}</a>`;
        });
    });
    nav += '<a class="sb-item" onclick="doLogout()" style="color:var(--danger);cursor:pointer"><i class="sb-icon">&#10140;</i>Logout</a>';

    document.getElementById('sidebar').innerHTML =
        '<div class="sb-brand"><span class="sb-logo">WPS</span><span class="sb-conn" id="conn-dot"></span></div>' +
        '<nav class="sb-nav">' + nav + '</nav>' +
        '<div class="sb-stats">' +
            '<div class="sb-stat-row"><span class="sb-stat-lbl">UPTIME</span><span class="sb-stat-val" id="sb-uptime">--:--:--</span></div>' +
            '<div class="sb-stat-row"><span class="sb-stat-lbl">RAM</span><span class="sb-stat-val" id="sb-ram">--%</span></div>' +
            '<div class="sb-bar-bg"><div class="sb-bar-fill" id="sb-ram-bar" style="width:0%"></div></div>' +
            '<div class="sb-stat-row"><span class="sb-stat-lbl">HEAP</span><span class="sb-stat-val" id="sb-heap">--</span></div>' +
            '<div class="sb-stat-row"><span class="sb-stat-lbl">STA</span><span class="sb-stat-val" id="sb-sta" style="font-size:0.85rem">--</span></div>' +
        '</div>';
}

function toggleSidebar() {
    document.getElementById('sidebar').classList.toggle('open');
    document.getElementById('sb-overlay').classList.toggle('show');
}

/* ---- AUTH ---- */
async function doLogout() {
    try { await fetch('/api/logout', { method: 'POST' }); } catch(e) {}
    window.location.href = '/login.html';
}

/* ---- TOAST ---- */
function toast(msg) {
    const t = document.getElementById('toast');
    t.innerText = msg;
    t.classList.add('show');
    setTimeout(() => t.classList.remove('show'), 3000);
}

/* ---- GLOBAL LOG ---- */
let globalLog = [];
try { globalLog = JSON.parse(sessionStorage.getItem('glog') || '[]'); } catch(e) {}

function glog(msg, category) {
    const t = new Date().toLocaleTimeString('it-IT', {hour:'2-digit',minute:'2-digit',second:'2-digit'});
    globalLog.push({ t, msg, cat: category || 'info' });
    if (globalLog.length > 2000) globalLog = globalLog.slice(-1000);
    try { sessionStorage.setItem('glog', JSON.stringify(globalLog.slice(-200))); } catch(e) {}
    renderGlobalLog();
}

let logFilter = 'all';
function renderGlobalLog() {
    const el = document.getElementById('global-log');
    if (!el) return;
    let entries = globalLog;
    if (logFilter !== 'all') entries = entries.filter(e => e.cat === logFilter);
    const slice = entries.slice(-300);
    el.innerHTML = slice.map(e => {
        let color = '';
        if (e.cat === 'ok') color = 'color:var(--accent)';
        else if (e.cat === 'err') color = 'color:var(--danger)';
        else if (e.cat === 'ws') color = 'color:var(--cyan)';
        return `<div style="${color}">[${e.t}] ${escHtml(e.msg)}</div>`;
    }).join('');
    el.scrollTop = el.scrollHeight;
}

/* ---- WEBSOCKET ---- */
function connectWS() {
    ws = new WebSocket(WS_URL);
    ws.onopen = () => {
        wsAuthFails = 0;
        connected = true;
        document.getElementById('conn-dot').classList.add('on');
        document.getElementById('conn-text').innerText = 'Online';
        glog('WebSocket connected', 'ok');
        if (statusTimer) clearInterval(statusTimer);
        statusTimer = setInterval(getStatus, 2000);
        if (onWsOpen) onWsOpen();
    };
    ws.onclose = () => {
        connected = false;
        document.getElementById('conn-dot').classList.remove('on');
        document.getElementById('conn-text').innerText = 'Offline';
        glog('WebSocket disconnected', 'err');
        if (statusTimer) clearInterval(statusTimer);
        if (onWsClose) onWsClose();
        wsAuthFails++;
        if (wsAuthFails > 3) { window.location.href = '/login.html'; return; }
        setTimeout(connectWS, 3000);
    };
    ws.onerror = () => ws.close();
    ws.onmessage = (e) => {
        try {
            const msg = JSON.parse(e.data);
            if (msg.type && (msg.type === 'ws' || msg.type === 'wr' || msg.type === 'we')) { if (onWpsPush) onWpsPush(msg); return; }
            if (msg.req_id && pending.has(msg.req_id)) {
                const { resolve, reject } = pending.get(msg.req_id);
                msg.status === 'error' ? reject(msg.message) : resolve(msg);
                pending.delete(msg.req_id);
            }
        } catch(err) { console.error(err); }
    };
}

function send(cmdId, params) {
    params = params || {};
    return new Promise((resolve, reject) => {
        if (!connected) { reject('Not connected'); return; }
        const id = ++reqId;
        pending.set(id, { resolve, reject });
        ws.send(JSON.stringify({ cmd: cmdId, req_id: id, ...params }));
        const tout = cmdId === CMD.WPS_START ? 30000 : 15000;
        setTimeout(() => { if (pending.has(id)) { pending.delete(id); reject('Timeout'); } }, tout);
    });
}

/* ---- STATUS ---- */
async function getStatus() {
    if (!connected) return;
    try {
        const d = await send(CMD.GET_STATUS);
        document.getElementById('sb-uptime').innerText = d.uptime || '--:--:--';
        const ram = d.ram || 0;
        document.getElementById('sb-ram').innerText = ram + '%';
        const bar = document.getElementById('sb-ram-bar');
        bar.style.width = ram + '%';
        bar.style.background = ram > 85 ? 'var(--danger)' : 'var(--accent)';
        document.getElementById('sb-heap').innerText = (d.free_heap || 0).toLocaleString();
        const staEl = document.getElementById('sb-sta');
        if (d.sta_connected && d.sta_ip) {
            staEl.innerText = d.sta_ip;
        } else {
            staEl.innerText = '--';
            staEl.style.color = 'var(--muted)';
        }
    } catch(e) {}
}

/* ---- UTILITIES ---- */
function escHtml(s) {
    const d = document.createElement('div');
    d.innerText = s;
    return d.innerHTML;
}

function initApp(activePage, title) {
    initSidebar(activePage);
    document.getElementById('page-title').innerText = title;
    connectWS();
}
