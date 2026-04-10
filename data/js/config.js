async function loadSettings() {
    try {
        const d = await send(CMD.GET_AP_SETTINGS);
        document.getElementById('cfg-ssid').value = d.ssid || '';
        document.getElementById('cfg-pass').value = d.password || '';
        document.getElementById('cfg-channel').value = d.channel || 1;
    } catch(e) {}
    try {
        const d = await send(CMD.GET_STA_SETTINGS);
        document.getElementById('sta-ssid').value = d.ssid || '';
        document.getElementById('sta-pass').value = d.password || '';
        const info = document.getElementById('sta-info');
        if (d.connected && d.ip) info.innerHTML = 'Connected: <span style="color:var(--accent)">' + d.ip + '</span>';
        else if (d.ssid) info.innerText = 'Not connected';
        else info.innerText = 'Not configured';
    } catch(e) {}
}

async function saveStaSettings(e) {
    e.preventDefault();
    try {
        const res = await send(CMD.SET_STA_SETTINGS, {
            ssid: document.getElementById('sta-ssid').value,
            password: document.getElementById('sta-pass').value
        });
        toast(res.message || 'Saved');
    } catch(err) { toast('Error: ' + err); }
}

async function saveSettings(e) {
    e.preventDefault();
    try {
        await send(CMD.SET_AP_SETTINGS, {
            ssid: document.getElementById('cfg-ssid').value,
            password: document.getElementById('cfg-pass').value,
            channel: parseInt(document.getElementById('cfg-channel').value)
        });
        toast('Settings saved! Reboot to apply.');
    } catch(err) { toast('Error: ' + err); }
}

async function saveWebPass(e) {
    e.preventDefault();
    const pass = document.getElementById('web-pass').value;
    if (pass.length < 4) { toast('Min 4 characters'); return; }
    try {
        const res = await send(CMD.SET_WEB_PASS, { password: pass });
        toast(res.message || 'Password changed');
        document.getElementById('web-pass').value = '';
    } catch(err) { toast('Error: ' + err); }
}

async function rebootDevice() {
    if (!confirm('Reboot?')) return;
    try { await send(CMD.REBOOT); toast('Rebooting...'); } catch(e) { toast('Reboot sent'); }
}

onWsOpen = function() { loadSettings(); };
