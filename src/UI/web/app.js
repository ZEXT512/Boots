// BOOTFPS — Cyber Sigilism frontend.
//
// Every panel below talks to the native bridge (src/UI/Bridge.cpp) which owns
// the real C++ modules. There is no mock data: if the host is offline the UI
// says so instead of inventing numbers.
(() => {
    "use strict";

    // ── DOM handles ──────────────────────────────────────────────────────
    const root = document.getElementById("view-root");
    const logEl = document.getElementById("log");
    const logCountEl = document.getElementById("log-count");
    const toastEl = document.getElementById("toast");
    const clockEl = document.getElementById("clock");
    const pillBridge = document.getElementById("pill-bridge");
    const pillAdmin = document.getElementById("pill-admin");
    const railHost = document.getElementById("rail-host");
    const railAdmin = document.getElementById("rail-admin");

    const MODULES = [
        "SystemInfoProvider", "HardwareMonitor", "ProcessManager",
        "RegistryOptimizer", "PowerOptimizer", "EmulatorOptimizer",
        "InputLatencyOptimizer", "PerformanceBenchmark",
    ];

    // ── State ────────────────────────────────────────────────────────────
    const state = {
        view: "dashboard",
        online: false,
        system: null,
        metrics: null,
        processCount: null,
        processes: null,
        processQuery: "",
        processSort: "cpu",
        registry: null,
        power: null,
        emulators: null,
        input: null,
        devices: null,
        latency: { running: false, stats: null },
        benchmark: {
            running: false, baseline: null, after: null,
            findings: null, rollback: null, report: null,
        },
        logs: [],
        timers: { metrics: null, processes: null, count: null },
    };

    // ── Native bridge plumbing ───────────────────────────────────────────
    const pending = new Map();
    let sequence = 0;
    let hostReady = false;
    let toastTimer = null;

    function hostAvailable() {
        return Boolean(window.chrome && window.chrome.webview &&
                       typeof window.chrome.webview.postMessage === "function");
    }

    function call(command, args = {}, timeoutMs = 120000) {
        if (!hostAvailable()) {
            return Promise.reject(new Error("WebView2 host bridge unavailable."));
        }
        const id = ++sequence;
        return new Promise((resolve, reject) => {
            const timer = setTimeout(() => {
                pending.delete(id);
                reject(new Error(`Native command timed out: ${command}`));
            }, timeoutMs);
            pending.set(id, { resolve, reject, timer });
            try {
                window.chrome.webview.postMessage({ id, command, args });
            } catch (error) {
                clearTimeout(timer);
                pending.delete(id);
                reject(error);
            }
        });
    }

    function wireHost() {
        if (!hostAvailable()) return;
        window.chrome.webview.addEventListener("message", (event) => {
            const message = event.data;
            if (!message || typeof message !== "object") return;
            if (message.type === "ready") {
                hostReady = true;
                updateStatus();
                return;
            }
            if (message.id === null || message.id === undefined) return;
            const entry = pending.get(message.id);
            if (!entry) return;
            pending.delete(message.id);
            clearTimeout(entry.timer);
            if (message.ok) entry.resolve(message.data || {});
            else entry.reject(new Error(message.error || "Native command failed."));
        });
    }

    const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

    async function waitForHost() {
        for (let attempt = 0; attempt < 24; attempt += 1) {
            try {
                const pong = await call("ping", {}, 2500);
                if (pong && pong.pong) return true;
            } catch (_) { /* host not ready yet */ }
            await sleep(400);
        }
        return false;
    }

    // ── Formatting helpers ───────────────────────────────────────────────
    function esc(value) {
        return String(value === null || value === undefined ? "" : value)
            .replace(/[&<>"']/g, (c) => ({
                "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;",
            }[c]));
    }

    const escAttr = esc;

    function num(value, digits = 0) {
        if (value === null || value === undefined || Number.isNaN(Number(value))) return "—";
        return Number(value).toFixed(digits);
    }

    function clamp(value, low = 0, high = 100) {
        return Math.min(high, Math.max(low, Number(value) || 0));
    }

    function fmtMb(mb) {
        if (mb === null || mb === undefined) return "—";
        return mb >= 1024 ? `${(mb / 1024).toFixed(1)} GB` : `${Math.round(mb)} MB`;
    }

    function fmtUptime(seconds) {
        const total = Math.max(0, Math.floor(Number(seconds) || 0));
        const days = Math.floor(total / 86400);
        const hours = Math.floor((total % 86400) / 3600);
        const minutes = Math.floor((total % 3600) / 60);
        if (days > 0) return `${days}d ${hours}h`;
        if (hours > 0) return `${hours}h ${minutes}m`;
        return `${minutes}m`;
    }

    // ── Activity log / toast ─────────────────────────────────────────────
    function logLine(kind, text) {
        const stamp = new Date().toTimeString().slice(0, 8);
        state.logs.push({ kind, text, stamp });
        if (state.logs.length > 250) state.logs.shift();
        renderLog();
    }

    function renderLog() {
        logEl.innerHTML = state.logs
            .map((line) => `<div class="log-line is-${line.kind}"><b>${line.stamp}</b> ${esc(line.text)}</div>`)
            .join("");
        logCountEl.textContent = `${state.logs.length} EVENT${state.logs.length === 1 ? "" : "S"}`;
        logEl.scrollTop = logEl.scrollHeight;
    }

    function toast(message, kind = "info") {
        toastEl.textContent = message;
        toastEl.className = `toast is-${kind}`;
        toastEl.hidden = false;
        clearTimeout(toastTimer);
        toastTimer = setTimeout(() => { toastEl.hidden = true; }, 4600);
    }

    function handleError(error) {
        const message = error && error.message ? error.message : String(error);
        logLine("error", message);
        toast(message, "error");
    }

    async function runAction(button, fn) {
        if (button && button.disabled) return;
        const label = button ? button.textContent : "";
        if (button) {
            button.disabled = true;
            button.textContent = "WORKING…";
        }
        try {
            await fn();
        } catch (error) {
            handleError(error);
        } finally {
            if (button && button.isConnected) {
                button.disabled = false;
                button.textContent = label;
            }
        }
    }

    // ── Status strip ─────────────────────────────────────────────────────
    function updateStatus() {
        if (pillBridge) {
            pillBridge.className = state.online ? "pill is-live" : "pill";
            pillBridge.innerHTML = `<i></i>${state.online ? "HOST LINKED" : (hostReady ? "HOST READY" : "HOST OFFLINE")}`;
        }
        const admin = state.system ? Boolean(state.system.admin) : null;
        if (pillAdmin) {
            pillAdmin.className = admin === null ? "pill" : (admin ? "pill pill-ok" : "pill is-warn");
            pillAdmin.innerHTML = `<i></i>${admin === null ? "ADMIN UNKNOWN" : (admin ? "ADMIN ELEVATED" : "STANDARD USER")}`;
        }
        if (railHost) railHost.textContent = state.online ? "LINKED" : "OFFLINE";
        if (railAdmin) railAdmin.textContent = admin === null ? "—" : (admin ? "YES" : "NO");
    }

    function setupClock() {
        const tick = () => { clockEl.textContent = new Date().toTimeString().slice(0, 8); };
        tick();
        setInterval(tick, 1000);
    }

    // ── Dashboard ────────────────────────────────────────────────────────
    function sigilSvg() {
        return `
        <div class="hero-sigil" aria-hidden="true">
            <svg viewBox="0 0 420 420">
                <g class="ring-a"><circle cx="210" cy="210" r="184"/></g>
                <g class="ring-b"><circle cx="210" cy="210" r="156" class="dash"/></g>
                <g class="ring-c"><circle cx="210" cy="210" r="126"/></g>
                <polygon points="210,52 348,292 72,292"/>
                <polygon points="210,368 72,128 348,128"/>
                <circle cx="210" cy="210" r="30" class="core-ring"/>
                <circle cx="210" cy="210" r="9" class="core"/>
                <g class="runes">
                    <text x="210" y="24">⟁</text>
                    <text x="404" y="216">0x05</text>
                    <text x="210" y="408">⟁</text>
                    <text x="16" y="216">SIGIL</text>
                </g>
            </svg>
            <div class="sigil-label">CORE // 0x05</div>
        </div>`;
    }

    function metricCard(id, label, foot) {
        return `
        <article class="metric">
            <div class="metric-label">${label}</div>
            <div class="metric-value" id="m-${id}">—</div>
            <div class="bar" id="m-${id}-wrap"><i id="m-${id}-bar"></i></div>
            <div class="metric-foot">${foot}</div>
        </article>`;
    }

    function systemInfoGrid(system) {
        if (!system) return `<div class="empty">Waiting for system.info…</div>`;
        const rows = [
            ["OS", system.os],
            ["HOST", system.hostname],
            ["USER", system.user],
            ["CPU", system.cpu],
            ["CORES", String(system.cores)],
            ["RAM", fmtMb(system.ram_total_mb)],
            ["UPTIME", fmtUptime(system.uptime_seconds)],
            ["ELEVATION", system.admin ? "ADMINISTRATOR" : "STANDARD USER"],
        ];
        return `<dl class="info-grid">${rows.map(([key, value]) => `
            <div><dt>${key}</dt><dd title="${escAttr(value)}">${esc(value)}</dd></div>`).join("")}
        </dl>`;
    }

    function renderDashboard() {
        const system = state.system;
        const modules = (system && system.modules) || MODULES;
        return `
        <div class="view">
            <section class="hero">
                <div class="hero-copy">
                    <div class="eyebrow">CORE ENGINE ${state.online ? "// LINKED" : "// OFFLINE"} // ${system && system.admin ? "ELEVATED" : "USER MODE"}</div>
                    <h2>${system ? esc(system.hostname) : "BOOTFPS"} PERFORMANCE CORE</h2>
                    <p>${system
                        ? `${esc(system.os)} — ${esc(system.cpu)} — ${system.cores} logical cores — ${fmtMb(system.ram_total_mb)} physical memory.`
                        : "Waiting for the native bridge to report this machine."}</p>
                    <div class="actions">
                        <button class="btn btn-primary" data-action="dash-refresh">RESAMPLE</button>
                        <button class="btn" data-action="dash-processes">PROCESS SENTINEL</button>
                        <button class="btn" data-action="dash-benchmark">BENCHMARK</button>
                    </div>
                </div>
                ${sigilSvg()}
            </section>

            <section class="metrics">
                ${metricCard("cpu", "CPU LOAD", "PDH / PROCESSOR")}
                ${metricCard("gpu", "GPU ENGINE", "PDH / GPU ENGINE")}
                ${metricCard("ram", "MEMORY", "GLOBAL MEMORY STATUS")}
                ${metricCard("disk", "DISK I/O", "PHYSICAL DISK / SEC")}
                ${metricCard("procs", "PROCESSES", "PROCESS SENTINEL")}
                ${metricCard("uptime", "UPTIME", "SYSTEM CLOCK")}
            </section>

            <section class="lower">
                <div class="panel">
                    <div class="panel-head"><span>CPU CORE MATRIX</span><span class="panel-tag" id="core-tag">—</span></div>
                    <div class="core-matrix" id="core-matrix"><div class="empty">Awaiting sample…</div></div>
                </div>
                <div class="panel">
                    <div class="panel-head"><span>SYSTEM PROFILE</span><span class="panel-tag">${system && system.admin ? "ADMIN" : "USER"}</span></div>
                    ${systemInfoGrid(system)}
                </div>
            </section>

            <section class="lower">
                <div class="panel">
                    <div class="panel-head"><span>MODULE STATUS</span><span class="panel-tag">${modules.length} MODULES</span></div>
                    <ul class="module-status">${modules.map((name) => `<li>${esc(name)}</li>`).join("")}</ul>
                </div>
                <div class="panel">
                    <div class="panel-head"><span>PERFORMANCE WARNINGS</span><span class="panel-tag" id="warn-tag">—</span></div>
                    <ul class="note-list" id="warn-list"><li>No sample yet.</li></ul>
                </div>
            </section>
        </div>`;
    }

    function setMetric(id, value, unit) {
        const el = document.getElementById(`m-${id}`);
        if (!el) return;
        el.innerHTML = value === null || value === undefined || value === "—"
            ? "—"
            : `${value}<small>${unit}</small>`;
    }

    function setBar(id, percent, warn) {
        const bar = document.getElementById(`m-${id}-bar`);
        const wrap = document.getElementById(`m-${id}-wrap`);
        if (bar) bar.style.width = `${clamp(percent)}%`;
        if (wrap) wrap.classList.toggle("is-warn", Boolean(warn));
    }

    function applyDashboard() {
        const metrics = state.metrics;
        const system = state.system;

        setMetric("cpu", metrics ? Math.round(metrics.cpu) : null, "%");
        setBar("cpu", metrics ? metrics.cpu : 0, metrics && metrics.cpu >= 85);

        if (metrics && metrics.gpu !== null && metrics.gpu !== undefined) {
            setMetric("gpu", Math.round(metrics.gpu), "%");
            setBar("gpu", metrics.gpu, metrics.gpu >= 90);
        } else {
            setMetric("gpu", null, "%");
            setBar("gpu", 0, false);
        }

        setMetric("ram", metrics ? Math.round(metrics.mem_percent) : null, "%");
        setBar("ram", metrics ? metrics.mem_percent : 0, metrics && metrics.mem_percent >= 85);

        if (metrics) {
            const throughput = (metrics.disk_read_mb_s || 0) + (metrics.disk_write_mb_s || 0);
            setMetric("disk", throughput.toFixed(1), "MB/S");
            setBar("disk", clamp(throughput * 2), throughput > 40);
        } else {
            setMetric("disk", null, "MB/S");
            setBar("disk", 0, false);
        }

        setMetric("procs", state.processCount, "");
        setBar("procs", state.processCount ? clamp(state.processCount / 4) : 0, false);

        setMetric("uptime", system ? fmtUptime(system.uptime_seconds) : null, "");
        setBar("uptime", 0, false);

        const coreTag = document.getElementById("core-tag");
        const coreMatrix = document.getElementById("core-matrix");
        if (coreTag && coreMatrix) {
            const perCore = (metrics && metrics.per_core) || [];
            coreTag.textContent = perCore.length ? `${perCore.length} CORES` : "N/A";
            coreMatrix.innerHTML = perCore.length
                ? perCore.map((value, index) => `
                    <div class="core-cell">
                        <span>C${String(index).padStart(2, "0")}<b>${Math.round(value)}%</b></span>
                        <div class="bar ${value >= 85 ? "is-warn" : ""}"><i style="width:${clamp(value)}%"></i></div>
                    </div>`).join("")
                : `<div class="empty">Per-core counters unavailable.</div>`;
        }

        const warnTag = document.getElementById("warn-tag");
        const warnList = document.getElementById("warn-list");
        if (warnTag && warnList) {
            const warnings = (metrics && metrics.warnings) || [];
            warnTag.textContent = warnings.length ? `${warnings.length} ACTIVE` : "CLEAR";
            warnList.innerHTML = warnings.length
                ? warnings.map((text) => `<li class="is-warn">${esc(text)}</li>`).join("")
                : `<li class="is-ok">No active performance warnings.</li>`;
        }
    }

    // ── Processes ────────────────────────────────────────────────────────
    function processRowsHtml() {
        const list = (state.processes && state.processes.processes) || [];
        const query = state.processQuery.trim().toLowerCase();
        const filtered = query
            ? list.filter((p) => String(p.name).toLowerCase().includes(query) || String(p.pid).includes(query))
            : list;

        if (!filtered.length) {
            return `<tr><td colspan="6" class="empty">No matching processes.</td></tr>`;
        }

        return filtered.map((p) => {
            const protectedProcess = p.class === "windows-critical" || p.class === "security-critical";
            return `
            <tr>
                <td class="num mono-dim">${p.pid}</td>
                <td class="name" title="${escAttr(p.name)}">${esc(p.name)}</td>
                <td><span class="badge ${escAttr(p.class)}">${esc(p.class)}</span></td>
                <td class="num">${num(p.cpu, 1)}</td>
                <td class="num">${p.mem_mb > 0 ? num(p.mem_mb, 1) : "—"}</td>
                <td>
                    <button class="btn btn-sm" data-action="toggle-whitelist"
                            data-name="${escAttr(p.name)}" data-whitelisted="${p.whitelisted}">
                        ${p.whitelisted ? "UNWHITELIST" : "WHITELIST"}
                    </button>
                    <button class="btn btn-sm btn-danger" data-action="kill-process"
                            data-pid="${p.pid}" data-name="${escAttr(p.name)}" data-class="${escAttr(p.class)}"
                            ${protectedProcess ? `disabled title="Protected system process"` : ""}>KILL</button>
                </td>
            </tr>`;
        }).join("");
    }

    function renderProcesses() {
        const data = state.processes;
        return `
        <div class="view">
            <section class="panel">
                <div class="toolbar">
                    <input class="input" id="process-search" data-role="process-search"
                           placeholder="FILTER NAME OR PID" value="${escAttr(state.processQuery)}" style="width:240px">
                    <select class="select" data-role="process-sort">
                        <option value="cpu" ${state.processSort === "cpu" ? "selected" : ""}>SORT: CPU</option>
                        <option value="mem" ${state.processSort === "mem" ? "selected" : ""}>SORT: MEMORY</option>
                        <option value="name" ${state.processSort === "name" ? "selected" : ""}>SORT: NAME</option>
                    </select>
                    <span class="mono-dim" id="process-meta">${data ? `${data.total} processes / showing ${data.shown}` : "scanning…"}</span>
                    <span class="spacer"></span>
                    <button class="btn btn-primary" data-action="process-refresh">RESCAN</button>
                </div>
                <div class="table-wrap">
                    <table class="data-table">
                        <thead><tr>
                            <th>PID</th><th>PROCESS</th><th>CLASS</th>
                            <th class="num">CPU %</th><th class="num">MEM MB</th><th>ACTIONS</th>
                        </tr></thead>
                        <tbody id="process-rows">${processRowsHtml()}</tbody>
                    </table>
                </div>
            </section>
        </div>`;
    }

    function updateProcessRows() {
        const body = document.getElementById("process-rows");
        if (body) body.innerHTML = processRowsHtml();
        const meta = document.getElementById("process-meta");
        if (meta && state.processes) {
            meta.textContent = `${state.processes.total} processes / showing ${state.processes.shown}`;
        }
    }

    async function loadProcesses(silent = false) {
        try {
            const data = await call("process.list", { limit: 400, sort: state.processSort });
            state.processes = data;
            state.processCount = data.total;
            if (state.view === "processes") {
                if (silent) updateProcessRows();
                else render();
            }
        } catch (error) {
            if (!silent) handleError(error);
        }
    }

    // ── Emulators ────────────────────────────────────────────────────────
    function renderEmulators() {
        const data = state.emulators;
        const instances = (data && data.instances) || [];
        return `
        <div class="view">
            <section class="panel">
                <div class="toolbar">
                    <span class="mono-dim">Detects MSI App Player / BlueStacks installations and running instances.</span>
                    <span class="spacer"></span>
                    <button class="btn btn-primary" data-action="emulator-detect">DETECT</button>
                </div>
                ${data === null
                    ? `<div class="empty">Scanning for emulators…</div>`
                    : (instances.length === 0
                        ? `<div class="empty">No MSI App Player or BlueStacks installation detected on this machine.</div>`
                        : `<div class="cards">${instances.map((instance, index) => `
                            <article class="card">
                                <div class="card-head">
                                    <div class="card-title">${esc(instance.name)}</div>
                                    <span class="badge ${instance.running ? "ok" : "background"}">${instance.running ? "RUNNING" : "INSTALLED"}</span>
                                </div>
                                <div class="card-sub" title="${escAttr(instance.install_dir)}">${esc(instance.install_dir)}</div>
                                <dl class="kv">
                                    <dt>VERSION</dt><dd>${esc(instance.version || "—")}</dd>
                                    <dt>PID</dt><dd>${instance.pid || "—"}</dd>
                                    <dt>CPU CORES</dt><dd>${instance.cpu_cores || "—"}</dd>
                                    <dt>RAM</dt><dd>${instance.ram_mb ? `${instance.ram_mb} MB` : "—"}</dd>
                                    <dt>RENDER</dt><dd>${esc(instance.rendering_mode || "—")}</dd>
                                    <dt>BACKEND</dt><dd>${esc(instance.graphics_backend || "—")}</dd>
                                    <dt>CONFIG</dt><dd>${instance.config_ok ? "READABLE" : esc(instance.config_error || "UNREADABLE")}</dd>
                                </dl>
                                <div class="card-actions">
                                    <button class="btn btn-sm btn-primary" data-action="emulator-profile" data-index="${index}" data-profile="CompetitiveGaming">COMPETITIVE</button>
                                    <button class="btn btn-sm" data-action="emulator-profile" data-index="${index}" data-profile="LowEndPC">LOW END</button>
                                    <button class="btn btn-sm" data-action="emulator-profile" data-index="${index}" data-profile="Streaming">STREAMING</button>
                                    <button class="btn btn-sm" data-action="emulator-backup" data-index="${index}">BACKUP</button>
                                    <button class="btn btn-sm" data-action="emulator-restore" data-index="${index}">RESTORE</button>
                                </div>
                            </article>`).join("")}</div>`)}
            </section>
        </div>`;
    }

    async function loadEmulators() {
        try {
            state.emulators = await call("emulator.detect");
            if (state.view === "emulator") render();
        } catch (error) {
            state.emulators = { instances: [] };
            handleError(error);
        }
    }

    // ── Registry ─────────────────────────────────────────────────────────
    function renderRegistry() {
        const data = state.registry;
        const entries = (data && data.entries) || [];
        return `
        <div class="view">
            <section class="panel">
                <div class="toolbar">
                    <span class="mono-dim">Curated safe-list only. HKLM writes require an elevated process.</span>
                    <span class="spacer"></span>
                    <button class="btn" data-action="registry-backup">BACKUP PROFILE</button>
                    <button class="btn btn-primary" data-action="registry-scan">SCAN</button>
                </div>
                ${data === null
                    ? `<div class="empty">Scanning managed registry keys…</div>`
                    : `<div class="table-wrap">
                        <table class="data-table">
                            <thead><tr>
                                <th>KEY</th><th>VALUE</th><th>CURRENT</th><th>PROPOSED</th>
                                <th>RISK</th><th>STATE</th><th>ACTIONS</th>
                            </tr></thead>
                            <tbody>${entries.map((entry) => `
                                <tr title="${escAttr(entry.explanation || "")}">
                                    <td class="name" title="${escAttr(`${entry.root}\\${entry.subkey}`)}">
                                        ${esc(entry.root)}<br><span class="mono-dim">${esc(entry.subkey)}</span>
                                    </td>
                                    <td>${esc(entry.value || "(default)")}<br><span class="mono-dim">${esc(entry.type)}</span></td>
                                    <td class="mono-dim">${entry.current === null ? "—" : esc(entry.current)}</td>
                                    <td>${entry.proposed === null ? "—" : esc(entry.proposed)}</td>
                                    <td><span class="badge ${escAttr(entry.risk)}">${esc(entry.risk)}</span></td>
                                    <td>${entry.modified ? `<span class="badge medium">MODIFIED</span>` : `<span class="badge ok">DEFAULT</span>`}
                                        ${entry.valid ? "" : `<span class="badge critical">INVALID</span>`}</td>
                                    <td>
                                        <button class="btn btn-sm btn-primary" data-action="registry-apply" data-index="${entry.index}"
                                                ${(!entry.modified || !entry.valid) ? "disabled" : ""}>APPLY</button>
                                        <button class="btn btn-sm" data-action="registry-restore" data-index="${entry.index}"
                                                ${!entry.valid ? "disabled" : ""}>RESTORE</button>
                                    </td>
                                </tr>`).join("")}</tbody>
                        </table>
                    </div>`}
            </section>
        </div>`;
    }

    async function loadRegistry() {
        try {
            state.registry = await call("registry.scan");
            if (state.view === "registry") render();
        } catch (error) {
            handleError(error);
        }
    }

    // ── Power ────────────────────────────────────────────────────────────
    function renderPower() {
        const data = state.power;
        const plans = (data && data.plans) || [];
        const current = plans.find((plan) => plan.active);
        const warnings = (data && data.warnings) || [];
        return `
        <div class="view">
            <section class="panel">
                <div class="panel-head">
                    <span>ACTIVE POWER SCHEME</span>
                    <span class="panel-tag">${current ? esc(current.name) : "UNKNOWN"}</span>
                </div>
                <div class="toolbar">
                    <span class="mono-dim">${current ? esc(current.guid) : "Enumerating schemes…"}</span>
                    <span class="spacer"></span>
                    <button class="btn" data-action="power-diagnose">DIAGNOSE</button>
                    <button class="btn btn-danger" data-action="power-restore">RESTORE ORIGINAL</button>
                    <button class="btn btn-primary" data-action="power-refresh">REFRESH</button>
                </div>
                <div class="control-grid">
                    ${["Balanced", "Performance", "Emulator", "Streaming"].map((profile) => `
                        <div class="control">
                            <div class="control-label">${profile.toUpperCase()} PROFILE</div>
                            <div class="card-sub">Duplicates a safe baseline and tunes processor settings for ${profile.toLowerCase()} workloads.</div>
                            <button class="btn btn-primary" data-action="power-profile" data-profile="${profile}">APPLY</button>
                        </div>`).join("")}
                </div>
            </section>

            <section class="lower">
                <div class="panel">
                    <div class="panel-head"><span>POWER SCHEMES</span><span class="panel-tag">${plans.length} FOUND</span></div>
                    <div class="table-wrap">
                        <table class="data-table">
                            <thead><tr><th>SCHEME</th><th>GUID</th><th>STATE</th><th>ACTION</th></tr></thead>
                            <tbody>${plans.map((plan) => `
                                <tr>
                                    <td>${esc(plan.name)}</td>
                                    <td class="mono-dim">${esc(plan.guid)}</td>
                                    <td>${plan.active ? `<span class="badge ok">ACTIVE</span>` : `<span class="badge background">IDLE</span>`}</td>
                                    <td><button class="btn btn-sm" data-action="power-set"
                                                data-guid="${escAttr(plan.guid)}" data-name="${escAttr(plan.name)}"
                                                ${plan.active ? "disabled" : ""}>ACTIVATE</button></td>
                                </tr>`).join("") || `<tr><td colspan="4" class="empty">No schemes reported.</td></tr>`}</tbody>
                        </table>
                    </div>
                </div>
                <div class="panel">
                    <div class="panel-head"><span>DIAGNOSTICS</span><span class="panel-tag">${warnings.length} NOTES</span></div>
                    <ul class="note-list">${warnings.length
                        ? warnings.map((text) => `<li class="is-warn">${esc(text)}</li>`).join("")
                        : `<li class="is-ok">No power-management warnings detected.</li>`}</ul>
                </div>
            </section>
        </div>`;
    }

    async function loadPower() {
        try {
            const [list, diagnose] = await Promise.all([
                call("power.list"),
                call("power.diagnose"),
            ]);
            state.power = { plans: list.plans || [], warnings: diagnose.warnings || [] };
            if (state.view === "power") render();
        } catch (error) {
            handleError(error);
        }
    }

    // ── Input ────────────────────────────────────────────────────────────
    function latencyChart(samples) {
        if (!samples || samples.length < 2) return "";
        const width = 900;
        const height = 120;
        const pad = 10;
        const min = Math.min(...samples);
        const max = Math.max(...samples);
        const span = Math.max(0.001, max - min);
        const step = (width - pad * 2) / (samples.length - 1);
        const points = samples.map((value, index) => {
            const x = pad + index * step;
            const y = height - pad - ((value - min) / span) * (height - pad * 2);
            return `${x.toFixed(1)},${y.toFixed(1)}`;
        }).join(" ");
        return `
        <svg class="latency-chart" viewBox="0 0 ${width} ${height}" preserveAspectRatio="none">
            <line x1="0" y1="${height / 2}" x2="${width}" y2="${height / 2}"></line>
            <polyline points="${points}"></polyline>
        </svg>`;
    }

    function renderInput() {
        const mouse = state.input ? state.input.mouse : null;
        const keyboard = state.input ? state.input.keyboard : null;
        const devices = (state.devices && state.devices.devices) || [];
        const stats = state.latency.stats;
        return `
        <div class="view">
            <section class="panel">
                <div class="toolbar">
                    <span class="mono-dim">Documented Windows parameters only — SPI mouse/keyboard settings and HID enumeration.</span>
                    <span class="spacer"></span>
                    <button class="btn" data-action="input-refresh">REFRESH</button>
                </div>
                <div class="control-grid">
                    <div class="control">
                        <div class="control-label">POINTER SPEED <b id="mouse-speed-value">${mouse ? mouse.speed : "—"}</b></div>
                        <input class="range" type="range" min="1" max="20" step="1"
                               value="${mouse ? mouse.speed : 10}" data-role="mouse-speed" ${mouse ? "" : "disabled"}>
                        <label class="check">
                            <input type="checkbox" id="mouse-accel" ${mouse && mouse.accel ? "checked" : ""} ${mouse ? "" : "disabled"}>
                            ENHANCE POINTER PRECISION
                        </label>
                        <button class="btn btn-primary" data-action="input-mouse-apply" ${mouse ? "" : "disabled"}>APPLY MOUSE</button>
                    </div>
                    <div class="control">
                        <div class="control-label">KEY REPEAT DELAY <b id="key-delay-value">${keyboard ? keyboard.delay : "—"}</b></div>
                        <input class="range" type="range" min="0" max="3" step="1"
                               value="${keyboard ? keyboard.delay : 1}" data-role="key-delay" ${keyboard ? "" : "disabled"}>
                        <div class="control-label">KEY REPEAT RATE <b id="key-rate-value">${keyboard ? keyboard.rate : "—"}</b></div>
                        <input class="range" type="range" min="0" max="31" step="1"
                               value="${keyboard ? keyboard.rate : 20}" data-role="key-rate" ${keyboard ? "" : "disabled"}>
                        <button class="btn btn-primary" data-action="input-keyboard-apply" ${keyboard ? "" : "disabled"}>APPLY KEYBOARD</button>
                    </div>
                    <div class="control">
                        <div class="control-label">INPUT PROFILES</div>
                        <div class="card-sub">Applies a curated mouse + keyboard combination.</div>
                        <button class="btn btn-primary" data-action="input-profile" data-profile="CompetitiveGaming">COMPETITIVE</button>
                        <button class="btn" data-action="input-profile" data-profile="EmulatorGaming">EMULATOR</button>
                        <button class="btn" data-action="input-profile" data-profile="StandardWindows">STANDARD</button>
                    </div>
                </div>
            </section>

            <section class="lower">
                <div class="panel">
                    <div class="panel-head"><span>HID INPUT DEVICES</span><span class="panel-tag">${devices.length} DEVICES</span></div>
                    <div class="table-wrap">
                        <table class="data-table">
                            <thead><tr><th>DESCRIPTION</th><th>TYPE</th><th>POWER SAVING</th><th>PATH</th></tr></thead>
                            <tbody>${devices.map((device) => `
                                <tr>
                                    <td>${esc(device.description || "Unknown device")}</td>
                                    <td><span class="badge ${device.is_mouse ? "user-app" : "background"}">${device.is_mouse ? "MOUSE" : (device.is_keyboard ? "KEYBOARD" : "HID")}</span></td>
                                    <td>${device.power_saving ? `<span class="badge medium">ENABLED</span>` : `<span class="badge ok">DISABLED</span>`}</td>
                                    <td class="mono-dim" title="${escAttr(device.path)}">${esc(device.path)}</td>
                                </tr>`).join("") || `<tr><td colspan="4" class="empty">No HID input devices reported.</td></tr>`}</tbody>
                        </table>
                    </div>
                </div>
                <div class="panel">
                    <div class="panel-head">
                        <span>LATENCY PROBE</span>
                        <span class="panel-tag">${state.latency.running ? "MEASURING" : (stats ? "COMPLETE" : "IDLE")}</span>
                    </div>
                    <div class="toolbar">
                        <button class="btn btn-primary" data-action="latency-start" ${state.latency.running ? "disabled" : ""}>START</button>
                        <button class="btn" data-action="latency-stop" ${state.latency.running ? "" : "disabled"}>STOP &amp; ANALYSE</button>
                        <span class="spacer"></span>
                        <span class="mono-dim">${state.latency.running ? "Move the mouse or type, then stop." : "Low-level hooks measure event intervals."}</span>
                    </div>
                    ${stats ? `
                        <div class="stat-row">
                            <div class="stat-box"><span>MIN MS</span><b>${num(stats.min_ms, 3)}</b></div>
                            <div class="stat-box"><span>AVG MS</span><b>${num(stats.average_ms, 3)}</b></div>
                            <div class="stat-box"><span>MAX MS</span><b>${num(stats.max_ms, 3)}</b></div>
                            <div class="stat-box"><span>STD DEV</span><b>${num(stats.stddev_ms, 3)}</b></div>
                        </div>
                        <div class="panel-body" style="padding-top:0">
                            <div class="mono-dim">${stats.count} samples captured.</div>
                            ${latencyChart(stats.samples)}
                        </div>` : `<div class="empty">No latency run yet.</div>`}
                </div>
            </section>
        </div>`;
    }

    async function loadInput() {
        try {
            const [config, devices] = await Promise.all([
                call("input.read"),
                call("input.devices"),
            ]);
            state.input = config;
            state.devices = devices;
            if (state.view === "input") render();
        } catch (error) {
            handleError(error);
        }
    }

    // ── Benchmark ────────────────────────────────────────────────────────
    function benchmarkCard(title, result, tag) {
        return `
        <article class="bench-card">
            <div class="panel-head"><span>${title}</span><span class="panel-tag">${tag}</span></div>
            ${result ? `<div class="bench-values">
                <div class="bench-stat"><span>AVG FPS</span><b>${num(result.avg_fps, 1)}</b></div>
                <div class="bench-stat"><span>1% LOW</span><b>${num(result.low_1_percent_fps, 1)}</b></div>
                <div class="bench-stat"><span>FRAME MS</span><b>${num(result.avg_frame_time_ms, 2)}</b></div>
                <div class="bench-stat"><span>FRAMES</span><b>${num(result.frames, 0)}</b></div>
                <div class="bench-stat"><span>CPU %</span><b>${num(result.cpu_usage_percent, 1)}</b></div>
                <div class="bench-stat"><span>GPU %</span><b>${num(result.gpu_usage_percent, 1)}</b></div>
                <div class="bench-stat"><span>RAM MB</span><b>${num(result.ram_usage_mb, 0)}</b></div>
                <div class="bench-stat"><span>DISK MB/S</span><b>${num(result.disk_activity_mb_s, 2)}</b></div>
            </div>
            <div class="panel-body" style="padding-top:0"><span class="mono-dim">${esc(result.timestamp)}</span></div>`
            : `<div class="empty">Not captured yet.</div>`}
        </article>`;
    }

    function renderBenchmark() {
        const benchmark = state.benchmark;
        const both = benchmark.baseline && benchmark.after;
        return `
        <div class="view">
            <section class="panel">
                <div class="toolbar">
                    <span class="mono-dim">Runs a real CPU frame workload, then compares the first run (baseline) with the latest run (after).</span>
                    <span class="spacer"></span>
                    <select class="select" data-role="benchmark-duration">
                        <option value="3">3 SECONDS</option>
                        <option value="5" selected>5 SECONDS</option>
                        <option value="10">10 SECONDS</option>
                        <option value="15">15 SECONDS</option>
                    </select>
                    <button class="btn btn-primary" data-action="benchmark-run" ${benchmark.running ? "disabled" : ""}>
                        ${benchmark.running ? "RUNNING…" : (benchmark.baseline ? "CAPTURE AFTER" : "CAPTURE BASELINE")}
                    </button>
                    <button class="btn" data-action="benchmark-reset" ${benchmark.running ? "disabled" : ""}>RESET</button>
                </div>
                ${benchmark.running ? `<div class="progress"><i></i></div>` : ""}
                <div class="bench-grid" style="padding:14px">
                    ${benchmarkCard("BASELINE", benchmark.baseline, "BEFORE")}
                    ${benchmarkCard("AFTER", benchmark.after, "AFTER")}
                </div>
            </section>

            <section class="lower">
                <div class="panel">
                    <div class="panel-head"><span>COMPARISON</span><span class="panel-tag">${both ? "READY" : "NEEDS 2 RUNS"}</span></div>
                    <div class="toolbar">
                        <button class="btn btn-primary" data-action="benchmark-compare" ${both ? "" : "disabled"}>COMPARE</button>
                        <button class="btn" data-action="benchmark-report" ${both ? "" : "disabled"}>WRITE REPORT</button>
                    </div>
                    <ul class="note-list">${(benchmark.findings || []).length
                        ? benchmark.findings.map((text) => `<li class="is-ok">${esc(text)}</li>`).join("")
                        : `<li>Run the benchmark twice, then compare.</li>`}</ul>
                </div>
                <div class="panel">
                    <div class="panel-head"><span>ROLLBACK ADVISORY</span><span class="panel-tag">${(benchmark.rollback || []).length} ITEMS</span></div>
                    <ul class="note-list">${(benchmark.rollback || []).length
                        ? benchmark.rollback.map((text) => `<li class="is-warn">${esc(text)}</li>`).join("")
                        : `<li class="is-ok">No rollback recommendations.</li>`}</ul>
                    ${benchmark.report ? `<div class="panel-body">
                        <div class="mono-dim">JSON: ${esc(benchmark.report.json_path)}</div>
                        <div class="mono-dim">HTML: ${esc(benchmark.report.html_path)}</div>
                    </div>` : ""}
                </div>
            </section>
        </div>`;
    }

    async function loadBenchmarkState() {
        try {
            const data = await call("benchmark.state");
            state.benchmark.baseline = data.baseline || null;
            state.benchmark.after = data.after || null;
            if (state.view === "benchmark") render();
        } catch (error) {
            handleError(error);
        }
    }

    // ── Renderers / loaders ──────────────────────────────────────────────
    const renderers = {
        dashboard: renderDashboard,
        processes: renderProcesses,
        emulator: renderEmulators,
        registry: renderRegistry,
        power: renderPower,
        input: renderInput,
        benchmark: renderBenchmark,
    };

    function render() {
        const renderer = renderers[state.view] || renderDashboard;
        root.innerHTML = renderer();
        if (state.view === "dashboard") applyDashboard();
    }

    function stopTimers() {
        clearInterval(state.timers.metrics);
        clearInterval(state.timers.processes);
        clearInterval(state.timers.count);
        state.timers.metrics = null;
        state.timers.processes = null;
        state.timers.count = null;
    }

    async function refreshMetrics() {
        try {
            state.metrics = await call("metrics.sample");
            if (state.view === "dashboard") applyDashboard();
        } catch (_) { /* transient while the sampler primes */ }
    }

    async function refreshProcessCount() {
        try {
            const data = await call("process.list", { limit: 1, sort: "cpu" });
            state.processCount = data.total;
            if (state.view === "dashboard") applyDashboard();
        } catch (_) { /* ignore */ }
    }

    function navigate(view) {
        if (!renderers[view]) view = "dashboard";
        state.view = view;
        document.querySelectorAll(".nav-item").forEach((button) => {
            button.classList.toggle("is-active", button.dataset.view === view);
        });
        stopTimers();
        render();

        if (view === "dashboard") {
            refreshMetrics();
            refreshProcessCount();
            state.timers.metrics = setInterval(refreshMetrics, 1500);
            state.timers.count = setInterval(refreshProcessCount, 8000);
        } else if (view === "processes") {
            loadProcesses();
            setTimeout(() => loadProcesses(true), 900); // second delta sample for real CPU %
            state.timers.processes = setInterval(() => loadProcesses(true), 5000);
        } else if (view === "emulator") {
            loadEmulators();
        } else if (view === "registry") {
            loadRegistry();
        } else if (view === "power") {
            loadPower();
        } else if (view === "input") {
            loadInput();
        } else if (view === "benchmark") {
            loadBenchmarkState();
        }
    }

    // ── Event handling ───────────────────────────────────────────────────
    root.addEventListener("input", (event) => {
        const el = event.target;
        const role = el.dataset.role;
        if (role === "process-search") {
            state.processQuery = el.value;
            updateProcessRows();
        } else if (role === "mouse-speed") {
            const label = document.getElementById("mouse-speed-value");
            if (label) label.textContent = el.value;
        } else if (role === "key-delay") {
            const label = document.getElementById("key-delay-value");
            if (label) label.textContent = el.value;
        } else if (role === "key-rate") {
            const label = document.getElementById("key-rate-value");
            if (label) label.textContent = el.value;
        }
    });

    root.addEventListener("change", (event) => {
        const el = event.target;
        if (el.dataset.role === "process-sort") {
            state.processSort = el.value;
            loadProcesses();
        }
    });

    root.addEventListener("click", async (event) => {
        const button = event.target.closest("[data-action]");
        if (!button) return;
        const action = button.dataset.action;

        if (action === "dash-refresh") {
            await runAction(button, async () => {
                await refreshMetrics();
                await refreshProcessCount();
                logLine("info", "Live sample refreshed.");
            });
        } else if (action === "dash-processes") {
            navigate("processes");
        } else if (action === "dash-benchmark") {
            navigate("benchmark");
        } else if (action === "process-refresh") {
            await runAction(button, async () => {
                await loadProcesses(true);
                logLine("info", "Process table rescanned.");
            });
        } else if (action === "kill-process") {
            const pid = Number(button.dataset.pid);
            const name = button.dataset.name;
            const cls = button.dataset.class;
            if (cls === "windows-critical" || cls === "security-critical") {
                toast("Protected system process — termination blocked.", "warn");
                return;
            }
            if (!window.confirm(`Terminate ${name} (PID ${pid})?`)) return;
            await runAction(button, async () => {
                await call("process.kill", { pid });
                logLine("ok", `Terminated ${name} (PID ${pid}).`);
                toast(`Terminated ${name}.`, "ok");
                await loadProcesses(true);
            });
        } else if (action === "toggle-whitelist") {
            const name = button.dataset.name;
            const whitelisted = button.dataset.whitelisted === "true";
            await runAction(button, async () => {
                await call("process.whitelist", { action: whitelisted ? "remove" : "add", name });
                logLine("ok", `${whitelisted ? "Removed" : "Added"} ${name} ${whitelisted ? "from" : "to"} the whitelist.`);
                await loadProcesses(true);
            });
        } else if (action === "emulator-detect") {
            await runAction(button, async () => {
                await loadEmulators();
                logLine("info", "Emulator detection complete.");
            });
        } else if (action === "emulator-profile") {
            const index = Number(button.dataset.index);
            const profile = button.dataset.profile;
            if (!window.confirm(`Back up the current configuration and apply the ${profile} profile?`)) return;
            await runAction(button, async () => {
                await call("emulator.profile", { index, profile });
                logLine("ok", `Emulator profile applied: ${profile}.`);
                toast(`Emulator profile applied: ${profile}.`, "ok");
                await loadEmulators();
            });
        } else if (action === "emulator-backup") {
            const index = Number(button.dataset.index);
            await runAction(button, async () => {
                await call("emulator.backup", { index });
                logLine("ok", "Emulator configuration backed up.");
                toast("Configuration backup created.", "ok");
            });
        } else if (action === "emulator-restore") {
            const index = Number(button.dataset.index);
            if (!window.confirm("Restore the most recent configuration backup?")) return;
            await runAction(button, async () => {
                await call("emulator.restore", { index });
                logLine("ok", "Emulator configuration restored.");
                toast("Configuration restored.", "ok");
                await loadEmulators();
            });
        } else if (action === "registry-scan") {
            await runAction(button, async () => {
                await loadRegistry();
                logLine("info", "Registry safe-list rescanned.");
            });
        } else if (action === "registry-backup") {
            await runAction(button, async () => {
                await call("registry.backup");
                logLine("ok", "Registry profile backup written.");
                toast("Registry profile backed up.", "ok");
            });
        } else if (action === "registry-apply") {
            const index = Number(button.dataset.index);
            if (!window.confirm("Apply this registry change? A backup is written first.")) return;
            await runAction(button, async () => {
                const result = await call("registry.apply", { index });
                logLine("ok", result.message || "Registry change applied.");
                toast(result.message || "Registry change applied.", "ok");
                await loadRegistry();
            });
        } else if (action === "registry-restore") {
            const index = Number(button.dataset.index);
            if (!window.confirm("Restore this registry value from the latest backup?")) return;
            await runAction(button, async () => {
                const result = await call("registry.restore", { index });
                logLine("ok", result.message || "Registry value restored.");
                toast(result.message || "Registry value restored.", "ok");
                await loadRegistry();
            });
        } else if (action === "power-refresh") {
            await runAction(button, async () => {
                await loadPower();
                logLine("info", "Power schemes refreshed.");
            });
        } else if (action === "power-set") {
            const guid = button.dataset.guid;
            const name = button.dataset.name;
            if (!window.confirm(`Activate the "${name}" power scheme?`)) return;
            await runAction(button, async () => {
                await call("power.set", { guid });
                logLine("ok", `Power scheme activated: ${name}.`);
                toast(`Activated ${name}.`, "ok");
                await loadPower();
            });
        } else if (action === "power-profile") {
            const profile = button.dataset.profile;
            if (!window.confirm(`Create and activate the ${profile} power profile?`)) return;
            await runAction(button, async () => {
                await call("power.profile", { profile });
                logLine("ok", `Power profile applied: ${profile}.`);
                toast(`Power profile applied: ${profile}.`, "ok");
                await loadPower();
            });
        } else if (action === "power-diagnose") {
            await runAction(button, async () => {
                await loadPower();
                logLine("info", "Power diagnostics refreshed.");
            });
        } else if (action === "power-restore") {
            if (!window.confirm("Restore the power scheme that was active before BOOTFPS started?")) return;
            await runAction(button, async () => {
                await call("power.restore");
                logLine("ok", "Original power scheme restored.");
                toast("Original power scheme restored.", "ok");
                await loadPower();
            });
        } else if (action === "input-refresh") {
            await runAction(button, async () => {
                await loadInput();
                logLine("info", "Input configuration refreshed.");
            });
        } else if (action === "input-mouse-apply") {
            const speed = Number(document.querySelector('[data-role="mouse-speed"]').value);
            const accel = document.getElementById("mouse-accel").checked;
            await runAction(button, async () => {
                await call("input.mouse", { speed, accel });
                logLine("ok", `Mouse settings applied (speed ${speed}, acceleration ${accel ? "on" : "off"}).`);
                toast("Mouse settings applied.", "ok");
                await loadInput();
            });
        } else if (action === "input-keyboard-apply") {
            const delay = Number(document.querySelector('[data-role="key-delay"]').value);
            const rate = Number(document.querySelector('[data-role="key-rate"]').value);
            await runAction(button, async () => {
                await call("input.keyboard", { delay, rate });
                logLine("ok", `Keyboard settings applied (delay ${delay}, rate ${rate}).`);
                toast("Keyboard settings applied.", "ok");
                await loadInput();
            });
        } else if (action === "input-profile") {
            const profile = button.dataset.profile;
            if (!window.confirm(`Apply the ${profile} input profile?`)) return;
            await runAction(button, async () => {
                await call("input.profile", { profile });
                logLine("ok", `Input profile applied: ${profile}.`);
                toast(`Input profile applied: ${profile}.`, "ok");
                await loadInput();
            });
        } else if (action === "latency-start") {
            await runAction(button, async () => {
                const result = await call("input.latency.start");
                state.latency.running = true;
                state.latency.stats = null;
                logLine("info", result.message || "Latency measurement started.");
                toast("Latency measurement started — move the mouse or type.", "ok");
                render();
            });
        } else if (action === "latency-stop") {
            await runAction(button, async () => {
                const stats = await call("input.latency.stop");
                state.latency.running = false;
                state.latency.stats = stats;
                logLine("ok", `Latency measured: avg ${num(stats.average_ms, 3)} ms over ${stats.count} samples.`);
                toast(`Latency captured: ${num(stats.average_ms, 3)} ms average.`, "ok");
                render();
            });
        } else if (action === "benchmark-run") {
            const duration = Number(document.querySelector('[data-role="benchmark-duration"]').value) || 5;
            state.benchmark.running = true;
            state.benchmark.findings = null;
            state.benchmark.rollback = null;
            render();
            try {
                const result = await call("benchmark.run", { seconds: duration }, duration * 1000 + 60000);
                state.benchmark.running = false;
                if (result.slot === "baseline") state.benchmark.baseline = result.result;
                else state.benchmark.after = result.result;
                logLine("ok", `Benchmark captured (${result.slot}): ${num(result.result.avg_fps, 1)} FPS.`);
                toast(`Benchmark captured: ${num(result.result.avg_fps, 1)} FPS.`, "ok");
            } catch (error) {
                state.benchmark.running = false;
                handleError(error);
            }
            if (state.view === "benchmark") render();
        } else if (action === "benchmark-compare") {
            await runAction(button, async () => {
                const result = await call("benchmark.compare");
                state.benchmark.findings = result.findings || [];
                state.benchmark.rollback = result.rollback || [];
                logLine("info", `Benchmark comparison produced ${state.benchmark.findings.length} findings.`);
                render();
            });
        } else if (action === "benchmark-report") {
            await runAction(button, async () => {
                const result = await call("benchmark.report");
                state.benchmark.report = { json_path: result.json_path, html_path: result.html_path };
                logLine("ok", `Report written: ${result.html_path}`);
                toast("Benchmark report written to ProgramData\\BOOTFPS\\reports.", "ok");
                render();
            });
        } else if (action === "benchmark-reset") {
            await runAction(button, async () => {
                await call("benchmark.reset");
                state.benchmark = {
                    running: false, baseline: null, after: null,
                    findings: null, rollback: null, report: null,
                };
                logLine("info", "Benchmark history cleared.");
                render();
            });
        }
    });

    // ── Boot sequence ────────────────────────────────────────────────────
    async function loadNativeLogTail() {
        try {
            const data = await call("logs.tail", { lines: 6 });
            (data.lines || []).forEach((line) => {
                const text = String(line).replace(/\s+\([^()]+:\d+\)$/, "");
                logLine("info", `native ${text}`);
            });
        } catch (_) { /* logging is best effort */ }
    }

    async function bootstrap() {
        setupClock();
        updateStatus();
        wireHost();

        document.querySelectorAll(".nav-item").forEach((button) => {
            button.addEventListener("click", () => navigate(button.dataset.view));
        });

        if (!hostAvailable()) {
            logLine("warn", "WebView2 host bridge unavailable — running outside BOOTFPS.");
            navigate("dashboard");
            return;
        }

        const linked = await waitForHost();
        if (!linked) {
            logLine("error", "Native bridge did not answer ping.");
            toast("Native bridge offline.", "error");
            navigate("dashboard");
            return;
        }

        state.online = true;
        updateStatus();

        try {
            state.system = await call("system.info");
            updateStatus();
            logLine("ok", `Native bridge linked — ${state.system.os}`);
        } catch (error) {
            handleError(error);
        }

        await loadNativeLogTail();
        navigate("dashboard");
    }

    bootstrap();
})();
