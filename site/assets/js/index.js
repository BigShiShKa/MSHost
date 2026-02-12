let statusInterval = null;
let logInterval = null;
let playerInterval = null;
let alertShown = false;
let isCommandProcessing = false;
const COMMAND_COOLDOWN = 1000;
let autoScrollEnabled = true;
let isUserScrolling = false;
let scrollTimeout = null;

// Текущий статус сервера (machine-readable)
let currentStatusCode = "unknown";
// Текущая роль пользователя
let currentRole = "user";
// Флаг: ядро выключается
let isShuttingDown = false;
let statusFailCount = 0;

// ── Хелпер авторизации ───────────────────────────────────────────

function getAuthHeader() {
    const login = localStorage.getItem("auth_login");
    const password = localStorage.getItem("auth_password");
    if (!login || !password) return null;
    return "Basic " + btoa(login + ":" + password);
}

window.addEventListener("DOMContentLoaded", async () => {
    const auth = getAuthHeader();

    if (!auth) {
        window.location.href = "auth.html";
        return;
    }

    // Валидация при загрузке страницы
    try {
        const res = await fetch("/api/status", {
            headers: { "Authorization": auth }
        });
        if (res.status === 401) {
            kickToAuth();
            return;
        }
        if (res.ok) {
            const data = await res.json();
            if (data.role) {
                currentRole = data.role;
            }
        }
    } catch (e) {
        // Сервер недоступен — показываем страницу, она сама покажет ошибки
    }

    applyRoleVisibility();

    const scrollBtn = document.querySelector('.scroll-down-btn');
    if (scrollBtn) scrollBtn.addEventListener('click', scrollLogsToBottom);

    const logWrapper = document.querySelector('.log-content-wrapper');
    if (logWrapper) logWrapper.addEventListener('scroll', handleLogScroll);

    updateScrollButtonVisibility();
    startStatusLoop();

    const cmdInput = document.getElementById('command');
    if (cmdInput) {
        cmdInput.addEventListener('keypress', function(e) {
            if (e.key === 'Enter') {
                e.preventDefault();
                sendCommand();
            }
        });
    }
});

// ── Управление видимостью по роли ──────────────────────────────

function applyRoleVisibility() {
    const adminElements = document.querySelectorAll('.admin-only');
    const display = currentRole === 'admin' ? '' : 'none';
    adminElements.forEach(el => el.style.display = display);
}

// ── Скролл логов ───────────────────────────────────────────────

function handleLogScroll() {
    isUserScrolling = true;
    updateScrollButtonVisibility();

    clearTimeout(scrollTimeout);
    scrollTimeout = setTimeout(() => {
        isUserScrolling = false;
    }, 100);
}

function updateScrollButtonVisibility() {
    const logWrapper = document.querySelector('.log-content-wrapper');
    const scrollBtn = document.querySelector('.scroll-down-btn');

    if (!logWrapper || !scrollBtn) return;

    const isAtBottom = logWrapper.scrollHeight - logWrapper.scrollTop <= logWrapper.clientHeight + 10;
    autoScrollEnabled = isAtBottom;

    scrollBtn.style.opacity = isAtBottom ? '0' : '1';
    scrollBtn.style.pointerEvents = isAtBottom ? 'none' : 'auto';
}

function scrollLogsToBottom() {
    const logWrapper = document.querySelector('.log-content-wrapper');
    if (!logWrapper) return;

    logWrapper.scrollTop = logWrapper.scrollHeight;
    autoScrollEnabled = true;
    updateScrollButtonVisibility();
}

// ── Обновление логов (admin only) ──────────────────────────────

async function updateLogs() {
    if (currentRole !== 'admin') return;

    const auth = getAuthHeader();
    if (!auth) return;

    try {
        const res = await fetch("/api/logs", { headers: { "Authorization": auth } });
        if (res.status === 401) return kickToAuth();
        if (res.status === 403) return;
        if (!res.ok) throw new Error(`HTTP ${res.status}`);

        const data = await res.json();
        const logBox = document.getElementById("log-box");
        const logWrapper = document.querySelector('.log-content-wrapper');

        if (!logBox || !logWrapper) return;

        const previousScrollTop = logWrapper.scrollTop;
        const previousScrollHeight = logWrapper.scrollHeight;
        const wasScrolledToBottom = previousScrollHeight - previousScrollTop <= logWrapper.clientHeight + 10;

        const text = Array.isArray(data.logs) ? data.logs.join("\n") : (data.logs || "Лог пуст");

        if (logBox.textContent !== text) {
            logBox.textContent = text;

            if (!wasScrolledToBottom && !isUserScrolling) {
                const newScrollHeight = logWrapper.scrollHeight;
                const heightDifference = newScrollHeight - previousScrollHeight;
                logWrapper.scrollTop = previousScrollTop + heightDifference;
            } else if (wasScrolledToBottom || autoScrollEnabled) {
                logWrapper.scrollTop = logWrapper.scrollHeight;
            }
        }

        updateScrollButtonVisibility();

    } catch (e) {
        console.error("[LOG]", e);
    }
}

// ── Обновление игроков ─────────────────────────────────────────

async function updatePlayers() {
    const auth = getAuthHeader();
    if (!auth) return;

    try {
        const res = await fetch('/api/players', {
            headers: { "Authorization": auth }
        });
        if (res.status === 401) return kickToAuth();
        if (!res.ok) throw new Error(`HTTP ${res.status}`);

        const data = await res.json();
        const countEl = document.getElementById("player-count");
        const listEl = document.getElementById("player-list");

        if (countEl) countEl.textContent = `${data.online} / ${data.max}`;

        if (listEl) {
            listEl.innerHTML = '';
            if (data.names && data.names.length > 0) {
                data.names.forEach(name => {
                    const item = document.createElement('div');
                    item.className = 'player-item';

                    const nameSpan = document.createElement('span');
                    nameSpan.className = 'player-name';
                    nameSpan.textContent = name;
                    item.appendChild(nameSpan);

                    if (currentRole === 'admin') {
                        const actions = document.createElement('div');
                        actions.className = 'player-actions';

                        const kickBtn = document.createElement('button');
                        kickBtn.className = 'player-action-btn kick-btn';
                        kickBtn.textContent = 'Kick';
                        kickBtn.onclick = () => playerAction('kick', name);

                        const banBtn = document.createElement('button');
                        banBtn.className = 'player-action-btn ban-btn';
                        banBtn.textContent = 'Ban';
                        banBtn.onclick = () => playerAction('ban', name);

                        actions.appendChild(kickBtn);
                        actions.appendChild(banBtn);
                        item.appendChild(actions);
                    }

                    listEl.appendChild(item);
                });
            } else {
                listEl.innerHTML = '<div class="no-players">Нет игроков онлайн</div>';
            }
        }
    } catch (e) {
        console.error("[PLAYERS]", e);
    }
}

async function playerAction(action, player) {
    const auth = getAuthHeader();
    if (!auth) return;

    const label = action === 'kick' ? 'кика' : 'бана';
    const reason = prompt(`Причина ${label} для ${player}:`, '');
    if (reason === null) return;

    try {
        const res = await fetch(`/api/${action}`, {
            method: 'POST',
            headers: {
                "Content-Type": "application/json",
                "Authorization": auth
            },
            body: JSON.stringify({ player, reason: reason || undefined })
        });

        if (res.status === 401) return kickToAuth();
        if (res.status === 403) { alert("Недостаточно прав"); return; }
        if (!res.ok) throw new Error(`HTTP ${res.status}`);

        const data = await res.json();
        alert(data.result || `${action} выполнен`);
        updatePlayers();
    } catch (e) {
        alert(`Ошибка: ${e.message}`);
    }
}

// ── Статусный цикл ─────────────────────────────────────────────

function startStatusLoop() {
    updateStatus();
    updateLogs();
    updatePlayers();

    statusInterval = setInterval(updateStatus, 2000);
    logInterval = setInterval(updateLogs, 5000);
    playerInterval = setInterval(updatePlayers, 10000);
}

function kickToAuth() {
    if (!alertShown) {
        alertShown = true;
        alert("Сессия истекла. Повторите вход.");
        localStorage.removeItem("auth_login");
        localStorage.removeItem("auth_password");
        window.location.href = "auth.html";
    }
}

// ── Скачивание модпака (admin only) ────────────────────────────

function downloadModpack() {
    const auth = getAuthHeader();
    if (!auth) return;

    document.getElementById("status").innerText = "Скачиваю модпак...";

    fetch("/api/download-modpack", {
        headers: { "Authorization": auth }
    })
    .then(res => {
        if (res.status === 401) { kickToAuth(); return; }
        if (res.status === 403) { alert("Недостаточно прав"); return; }
        if (!res.ok) throw new Error(`HTTP ${res.status}`);

        const disposition = res.headers.get("Content-Disposition") || "";
        const match = disposition.match(/filename=(.+)/);
        const filename = match ? match[1].replace(/"/g, '') : "modpack.zip";
        return res.blob().then(blob => ({ blob, filename }));
    })
    .then(result => {
        if (!result) return;
        const { blob, filename } = result;
        const url = URL.createObjectURL(blob);
        const a = document.createElement("a");
        a.href = url;
        a.download = filename;
        a.style.display = "none";
        document.body.appendChild(a);
        a.click();
        document.body.removeChild(a);
        URL.revokeObjectURL(url);
    })
    .catch(e => {
        console.error("[DOWNLOAD]", e);
        document.getElementById("status").innerText = "Ошибка скачивания модпака";
    });
}

// ── Отправка команды (admin only) ──────────────────────────────

async function sendCommand() {
    if (currentRole !== 'admin') return;
    if (isCommandProcessing) return;

    const auth = getAuthHeader();
    const cmdInput = document.getElementById("command");
    const cmd = cmdInput.value.trim();

    if (currentStatusCode !== "running") {
        alert("Ошибка отправки команды! Сервер не запущен.");
        cmdInput.value = "";
        return;
    }

    if (!cmd) {
        alert("Введите команду");
        return;
    }

    isCommandProcessing = true;

    try {
        if (cmd === "stop") {
            await send('/api/stop');
        } else {
            const res = await fetch('/api/command', {
                method: 'POST',
                headers: {
                    "Content-Type": "application/json",
                    "Authorization": auth
                },
                body: JSON.stringify({ command: cmd })
            });

            if (res.status === 401) kickToAuth();
            if (res.status === 403) { alert("Недостаточно прав"); return; }
            if (!res.ok) throw new Error(`Ошибка: ${res.status}`);

            const data = await res.json();
            document.getElementById("status").innerText =
                "Ответ: " + (data.status_code === "running" ? "Выполнено!" : "Сервер отключен!");
        }

        cmdInput.value = "";
    } catch (e) {
        document.getElementById("status").innerText = "Ошибка при отправке команды";
        console.error(e);
    } finally {
        setTimeout(() => {
            isCommandProcessing = false;
        }, COMMAND_COOLDOWN);
    }
}

// ── Обновление статуса ─────────────────────────────────────────

async function updateStatus() {
    if (isShuttingDown) return;

    const auth = getAuthHeader();
    if (!auth) return;

    try {
        const res = await fetch('/api/status', {
            headers: { "Authorization": auth }
        });

        if (res.status === 401) return kickToAuth();
        if (!res.ok) throw new Error(`Ошибка: ${res.status}`);

        statusFailCount = 0;

        const data = await res.json();

        currentStatusCode = data.status_code || "unknown";

        // Обновляем роль и видимость
        if (data.role && data.role !== currentRole) {
            currentRole = data.role;
            applyRoleVisibility();
        }

        document.getElementById("status").innerText = "Статус: " + (data.status || "Неизвестно");
        document.getElementById("server-ip").innerText = data.ip || "—";
        document.getElementById("server-port").innerText = data.port || "—";
        document.getElementById("server-version").innerText = data.version || "—";

        const connectEl = document.getElementById("connect-address");
        if (connectEl && data.ip && data.port) {
            connectEl.innerText = data.ip + ":" + data.port;
        }
    } catch (e) {
        statusFailCount++;
        // После 3 подряд неудачных запросов — показываем страницу выключения
        if (statusFailCount >= 3) {
            isShuttingDown = true;
            showShutdownPage();
            return;
        }
        document.getElementById("status").innerText = "Ошибка при получении статуса";
        document.getElementById("server-ip").innerText = "—";
        document.getElementById("server-port").innerText = "—";
        document.getElementById("server-version").innerText = "—";
    }
}

// ── Страница выключения ──────────────────────────────────────

function showShutdownPage() {
    if (document.getElementById('shutdown-overlay')) return;

    // Останавливаем все интервалы
    if (statusInterval) { clearInterval(statusInterval); statusInterval = null; }
    if (logInterval) { clearInterval(logInterval); logInterval = null; }
    if (playerInterval) { clearInterval(playerInterval); playerInterval = null; }

    const overlay = document.createElement('div');
    overlay.id = 'shutdown-overlay';
    overlay.style.cssText = `
        position: fixed; top: 0; left: 0; width: 100%; height: 100%;
        background: linear-gradient(135deg, #1a1a2e 0%, #16213e 50%, #0f3460 100%);
        display: flex; flex-direction: column; align-items: center; justify-content: center;
        z-index: 9999; color: #e0e0e0; font-family: 'Segoe UI', Roboto, sans-serif;
        animation: fadeIn 0.5s ease-out;
    `;
    overlay.innerHTML = `
        <div style="text-align: center; padding: 40px;">
            <div style="font-size: 64px; margin-bottom: 24px;">&#x1F6D1;</div>
            <h1 style="font-size: 2rem; margin-bottom: 16px; background: linear-gradient(90deg, #ff6464, #ff9080); -webkit-background-clip: text; background-clip: text; color: transparent;">
                Сервер выключается
            </h1>
            <p style="font-size: 1.1rem; color: #a0a0b0; max-width: 400px; line-height: 1.6;">
                MSHost завершает работу.<br>До встречи!
            </p>
        </div>
    `;
    document.body.appendChild(overlay);
}

// ── Выключение ядра (admin only) ──────────────────────────────

async function exitProgram() {
    if (currentRole !== 'admin') return;
    if (!confirm("Вы уверены? Это полностью выключит MSHost.")) return;

    const auth = getAuthHeader();
    if (!auth) return;

    isShuttingDown = true;

    try {
        await fetch('/api/exit', {
            method: 'POST',
            headers: { "Authorization": auth }
        });
    } catch (e) {
        // Сервер мог уже упасть — это нормально
    }

    showShutdownPage();
}

// ── Отправка POST-действия ─────────────────────────────────────

async function send(path) {
    const auth = getAuthHeader();
    if (!auth) return alert("Авторизуйтесь для выполнения действия");

    try {
        const res = await fetch(path, {
            method: 'POST',
            headers: { "Authorization": auth }
        });

        if (res.status === 401) return kickToAuth();
        if (res.status === 403) {
            alert("Недостаточно прав для выполнения действия");
            return;
        }
        if (!res.ok) throw new Error(`Ошибка: ${res.status}`);

        const data = await res.json();
        document.getElementById("status").innerText = "Статус: " + (data.status || "Нет сообщения");
    } catch (e) {
        document.getElementById("status").innerText = "Ошибка при выполнении команды";
    }
}
