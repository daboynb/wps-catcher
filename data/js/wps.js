let wpsListening = false;
let wpsPollTimer = null, wpsClockTimer = null, wpsStartedAt = 0, wpsTimeoutSec = 0;

/* ---- WPS LISTENER ---- */

async function wpsStart() {
    if (wpsListening) { wpsStop(); return; }
    const timeout = parseInt(document.getElementById('wps-timeout').value);
    wlog('WPS START: timeout=' + formatTimeout(timeout));
    try {
        await send(CMD.WPS_START, { timeout: timeout });
        wpsListening = true;
        wpsUpdateButtons();
        document.getElementById('wps-status-box').style.display = '';
        document.getElementById('wps-state').innerHTML = '<span style="color:var(--amber)">LISTENING...</span>';
        document.getElementById('wps-result').style.display = 'none';
        wpsClockStart(timeout);
        wpsPollStart();
        wlog('WPS listening (any network)...', 'log-ok');
        glog('WPS listener active: ANY timeout=' + formatTimeout(timeout), 'ok');
    } catch(e) { wlog('WPS START FAILED: ' + e, 'log-err'); toast('WPS error: ' + e); }
}

function wpsClockStart(timeoutSec) {
    wpsStartedAt = Date.now();
    wpsTimeoutSec = timeoutSec;
    wpsClockStop();
    wpsClockTick();
    wpsClockTimer = setInterval(wpsClockTick, 1000);
}

function wpsClockStop() {
    if (wpsClockTimer) { clearInterval(wpsClockTimer); wpsClockTimer = null; }
}

function wpsClockTick() {
    const elapsed = Math.floor((Date.now() - wpsStartedAt) / 1000);
    const el = document.getElementById('wps-elapsed');
    if (!el) return;
    const fmt = (s) => {
        if (s >= 3600) return Math.floor(s/3600) + 'h ' + Math.floor((s%3600)/60) + 'm';
        if (s >= 60) return Math.floor(s/60) + 'm ' + (s%60) + 's';
        return s + 's';
    };
    el.innerText = fmt(elapsed) + ' / ' + (wpsTimeoutSec > 0 ? formatTimeout(wpsTimeoutSec) : 'no limit');
}

function wpsPollStart() {
    wpsPollStop();
    let polling = false;
    wpsPollTimer = setInterval(async () => {
        if (!wpsListening) { wpsPollStop(); return; }
        if (!connected || polling) return;
        polling = true;
        try {
            const d = await send(CMD.WPS_STATUS);
            if (d.state === 'listening') {
                wpsStartedAt = Date.now() - (d.elapsed * 1000);
            } else if (d.state === 'success') {
                wpsClockStop();
                document.getElementById('wps-state').innerHTML = '<span style="color:var(--accent)">SUCCESS!</span>';
                document.getElementById('wps-elapsed').innerText = 'Credentials captured!';
                if (d.ssid) {
                    document.getElementById('wps-result').style.display = '';
                    document.getElementById('wps-cap-ssid').innerText = d.ssid;
                    document.getElementById('wps-cap-pass').innerText = d.password || '(empty)';
                    document.getElementById('wps-cap-bssid').innerText = d.bssid || '';
                    wlog('CAPTURED: SSID=' + d.ssid + ' PASS=' + d.password + ' BSSID=' + d.bssid, 'log-ok');
                    glog('WPS CAPTURED: ' + d.ssid, 'ok');
                }
                wpsListening = false;
                wpsUpdateButtons();
                wpsPollStop();
                loadSavedWifi();
            } else if (d.state === 'failed' || d.state === 'timeout' || d.state === 'idle') {
                wpsClockStop();
                const label = d.state === 'timeout' ? 'TIMEOUT' : d.state === 'failed' ? 'FAILED' : 'STOPPED';
                document.getElementById('wps-state').innerHTML = '<span style="color:var(--muted)">' + label + '</span>';
                wpsListening = false;
                wpsUpdateButtons();
                wpsPollStop();
            }
        } catch(e) {}
        polling = false;
    }, 5000);
}

function wpsPollStop() {
    if (wpsPollTimer) { clearInterval(wpsPollTimer); wpsPollTimer = null; }
}

async function wpsStop() {
    wlog('WPS STOP requested');
    wpsPollStop();
    wpsClockStop();
    try { await send(CMD.WPS_STOP); } catch(e) {}
    wpsListening = false;
    wpsUpdateButtons();
    wlog('WPS listener stopped', 'log-warn');
    glog('WPS listener stopped', 'info');
}

function wpsUpdateButtons() {
    document.getElementById('btn-wps-start').style.display = wpsListening ? 'none' : '';
    document.getElementById('btn-wps-stop').style.display = wpsListening ? '' : 'none';
}

function formatTimeout(sec) {
    if (sec === 0) return 'no limit';
    if (sec < 3600) return Math.floor(sec/60) + 'min';
    return Math.floor(sec/3600) + 'h';
}

/* ---- WPS LOG ---- */
function wlog(msg, cls) {
    const el = document.getElementById('wps-log');
    if (!el) return;
    const t = new Date().toLocaleTimeString('it-IT',{hour:'2-digit',minute:'2-digit',second:'2-digit'});
    const div = document.createElement('div');
    div.className = cls || '';
    div.textContent = '[' + t + '] ' + msg;
    el.appendChild(div);
    el.scrollTop = el.scrollHeight;
}
function clearWpsLog() { const el = document.getElementById('wps-log'); if(el) el.innerHTML = ''; }

/* ---- WPS PUSH HANDLER ---- */
onWpsPush = function(msg) {
    switch(msg.type) {
    case 'ws':
        wlog('[ESP] ' + msg.phase + ': ' + msg.message, msg.phase === 'success' ? 'log-ok' : msg.phase === 'timeout' ? 'log-err' : 'log-info');
        glog('WPS: ' + msg.message, 'info');
        if (msg.phase === 'listening') {
            document.getElementById('wps-state').innerHTML = '<span style="color:var(--amber)">LISTENING...</span>';
            document.getElementById('wps-elapsed').innerText = msg.elapsed + 's / ' + formatTimeout(msg.timeout);
        } else if (msg.phase === 'success') {
            document.getElementById('wps-state').innerHTML = '<span style="color:var(--accent)">SUCCESS!</span>';
            wpsListening = false; wpsUpdateButtons();
        } else if (msg.phase === 'timeout') {
            document.getElementById('wps-state').innerHTML = '<span style="color:var(--muted)">TIMEOUT</span>';
            document.getElementById('wps-elapsed').innerText = 'No WPS button press detected';
            wpsListening = false; wpsUpdateButtons();
        } else if (msg.phase === 'stopped') {
            document.getElementById('wps-state').innerHTML = '<span style="color:var(--muted)">STOPPED</span>';
            wpsListening = false; wpsUpdateButtons();
        } else if (msg.phase === 'started') {
            document.getElementById('wps-status-box').style.display = '';
            document.getElementById('wps-state').innerHTML = '<span style="color:var(--amber)">LISTENING...</span>';
        } else if (msg.phase === 'retry' || msg.phase === 'mismatch') {
            document.getElementById('wps-state').innerHTML = '<span style="color:var(--amber)">' + msg.message.toUpperCase() + '</span>';
        }
        break;
    case 'wr':
        wlog('CAPTURED! SSID=' + msg.ssid + ' PASS=' + msg.password + ' BSSID=' + msg.bssid, 'log-ok');
        glog('WPS CAPTURED: SSID=' + msg.ssid + ' PASS=' + msg.password, 'ok');
        document.getElementById('wps-result').style.display = '';
        document.getElementById('wps-cap-ssid').innerText = msg.ssid;
        document.getElementById('wps-cap-pass').innerText = msg.password;
        document.getElementById('wps-cap-bssid').innerText = msg.bssid;
        toast('WPS credentials captured!');
        loadSavedWifi();
        break;
    case 'we':
        wlog('ERROR: ' + msg.message, 'log-err');
        glog('WPS ERROR: ' + msg.message, 'err');
        toast('WPS: ' + msg.message);
        break;
    }
};

/* ---- SAVED PASSWORDS ---- */
let savedWifiEntries = [];

async function loadSavedWifi() {
    const list = document.getElementById('saved-wifi-list');
    list.innerHTML = '<div style="color:var(--muted);font-size:0.9rem;padding:8px 0">Loading...</div>';
    try {
        const d = await send(CMD.SAVED_WIFI_LIST);
        savedWifiEntries = d.entries || [];
        renderSavedWifi();
    } catch(e) {
        list.innerHTML = '<div style="color:var(--danger);font-size:0.9rem;padding:8px 0">Failed to load</div>';
    }
}

function renderSavedWifi() {
    const list = document.getElementById('saved-wifi-list');
    const count = document.getElementById('saved-wifi-count');
    count.innerText = savedWifiEntries.length;

    if (savedWifiEntries.length === 0) {
        list.innerHTML = '<div style="color:var(--muted);font-size:0.9rem;padding:12px 0">No saved passwords yet. Capture WiFi credentials via WPS to see them here.</div>';
        return;
    }

    list.innerHTML = savedWifiEntries.map((e, i) => `
        <div class="saved-wifi-card">
            <div class="saved-wifi-info">
                <div class="saved-wifi-ssid">${escHtml(e.ssid)}</div>
                <div class="saved-wifi-bssid">${escHtml(e.bssid || 'N/A')}</div>
            </div>
            <div class="saved-wifi-pass">
                <span class="saved-wifi-pass-text" id="sw-pass-${i}" data-hidden="true">&#8226;&#8226;&#8226;&#8226;&#8226;&#8226;&#8226;&#8226;</span>
                <button class="btn btn-secondary saved-wifi-btn" onclick="toggleSavedPass(${i})" title="Show/Hide">&#9737;</button>
            </div>
            <div class="saved-wifi-actions">
                <button class="btn btn-secondary saved-wifi-btn" onclick="copySavedPass(${i})" title="Copy password">&#10064;</button>
                <button class="btn btn-danger saved-wifi-btn" onclick="deleteSavedWifi(${i})" title="Delete">&#10005;</button>
            </div>
        </div>
    `).join('');
}

function toggleSavedPass(idx) {
    const el = document.getElementById('sw-pass-' + idx);
    if (!el) return;
    const e = savedWifiEntries[idx];
    if (!e) return;
    if (el.dataset.hidden === 'true') {
        el.innerText = e.password || '(empty)';
        el.style.color = 'var(--accent)';
        el.dataset.hidden = 'false';
    } else {
        el.innerText = '\u2022\u2022\u2022\u2022\u2022\u2022\u2022\u2022';
        el.style.color = '';
        el.dataset.hidden = 'true';
    }
}

async function copySavedPass(idx) {
    const e = savedWifiEntries[idx];
    if (!e) return;
    try {
        await navigator.clipboard.writeText(e.password);
        toast('Password copied!');
    } catch(err) {
        const ta = document.createElement('textarea');
        ta.value = e.password;
        ta.style.position = 'fixed';
        ta.style.opacity = '0';
        document.body.appendChild(ta);
        ta.select();
        document.execCommand('copy');
        document.body.removeChild(ta);
        toast('Password copied!');
    }
}

async function deleteSavedWifi(idx) {
    const e = savedWifiEntries[idx];
    if (!e) { toast('Entry not found, refreshing...'); loadSavedWifi(); return; }
    if (!confirm('Delete saved password for "' + e.ssid + '"?')) return;
    try {
        await send(CMD.SAVED_WIFI_DELETE, { bssid: e.bssid });
        toast('Entry deleted');
        loadSavedWifi();
    } catch(err) {
        toast('Delete failed');
    }
}

/* ---- INIT ---- */
onWsOpen = function() {
    loadSavedWifi();
    /* Check if WPS is already running */
    (async function() {
        try {
            const d = await send(CMD.WPS_STATUS);
            if (d.state === 'listening') {
                wpsListening = true;
                wpsUpdateButtons();
                document.getElementById('wps-status-box').style.display = '';
                document.getElementById('wps-state').innerHTML = '<span style="color:var(--amber)">LISTENING...</span>';
                document.getElementById('wps-elapsed').innerText = d.elapsed + 's / ' + formatTimeout(d.timeout);
                wpsClockStart(d.timeout);
                wpsStartedAt = Date.now() - (d.elapsed * 1000);
                wpsPollStart();
            } else if (d.state === 'success' && d.ssid) {
                document.getElementById('wps-status-box').style.display = '';
                document.getElementById('wps-state').innerHTML = '<span style="color:var(--accent)">SUCCESS!</span>';
                document.getElementById('wps-result').style.display = '';
                document.getElementById('wps-cap-ssid').innerText = d.ssid;
                document.getElementById('wps-cap-pass').innerText = d.password;
                document.getElementById('wps-cap-bssid').innerText = d.bssid;
            }
        } catch(e) {}
    })();
};
