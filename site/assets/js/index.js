let statusInterval = null;
let logInterval = null;
let alertShown = false;
let isCommandProcessing = false;
const COMMAND_COOLDOWN = 1000;
let autoScrollEnabled = true;
let isUserScrolling = false;
let scrollTimeout = null;

// Текущий статус сервера (machine-readable)
let currentStatusCode = "unknown";

window.addEventListener("DOMContentLoaded", async () => {
    const token = localStorage.getItem("api_token");

    if (!token) {
        window.location.href = "auth.html";
        return;
    }

    // Валидация токена при загрузке страницы
    try {
        const res = await fetch("/api/status", {
            headers: { "X-API-Token": token }
        });
        if (res.status === 401) {
            localStorage.removeItem("api_token");
            window.location.href = "auth.html";
            return;
        }
    } catch (e) {
        // Сервер недоступен — показываем страницу, она сама покажет ошибки
    }

    const scrollBtn = document.querySelector('.scroll-down-btn');
    scrollBtn.addEventListener('click', scrollLogsToBottom);

    document.querySelector('.log-content-wrapper').addEventListener('scroll', handleLogScroll);
    updateScrollButtonVisibility();
    startStatusLoop();

    document.getElementById('command').addEventListener('keypress', function(e) {
        if (e.key === 'Enter') {
            e.preventDefault();
            sendCommand();
        }
    });
});

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

async function updateLogs() {
    const token = localStorage.getItem("api_token");
    if (!token) return;

    try {
        const res = await fetch("/api/logs", { headers: { "X-API-Token": token } });
        if (res.status === 401) return kickToAuth();
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
        const logBox = document.getElementById("log-box");
        if (logBox) {
            logBox.textContent = "Не удалось загрузить логи";
        }
    }
}

function startStatusLoop() {
    updateStatus();
    updateLogs();

    // Статус — каждые 2 сек, логи — каждые 5 сек
    statusInterval = setInterval(updateStatus, 2000);
    logInterval = setInterval(updateLogs, 5000);
}

function kickToAuth() {
    if (!alertShown) {
        alertShown = true;
        alert("Неверный API Token! Повторите вход.");
        localStorage.removeItem("api_token");
        window.location.href = "auth.html";
    }
}

function downloadModpack() {
    const token = localStorage.getItem("api_token");
    if (!token) return;

    // Скачивание через fetch + Blob (токен не попадает в URL)
    document.getElementById("status").innerText = "Скачиваю модпак...";

    fetch("/api/download-modpack", {
        headers: { "X-API-Token": token }
    })
    .then(res => {
        if (res.status === 401) { kickToAuth(); return; }
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

async function sendCommand() {
    if (isCommandProcessing) return;

    const token = localStorage.getItem("api_token");
    const cmdInput = document.getElementById("command");
    const cmd = cmdInput.value.trim();

    // Проверяем по machine-readable статусу
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
                    "X-API-Token": token
                },
                body: JSON.stringify({ command: cmd })
            });

            if (res.status === 401) kickToAuth();
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

async function updateStatus() {
    const token = localStorage.getItem("api_token");
    if (!token) return;

    try {
        const res = await fetch('/api/status', {
            headers: { "X-API-Token": token }
        });

        if (res.status === 401) return kickToAuth();
        if (!res.ok) throw new Error(`Ошибка: ${res.status}`);

        const data = await res.json();

        // Сохраняем machine-readable статус для логики
        currentStatusCode = data.status_code || "unknown";

        document.getElementById("status").innerText = "Статус: " + (data.status || "Неизвестно");
        document.getElementById("server-ip").innerText = data.ip || "—";
        document.getElementById("server-port").innerText = data.port || "—";
        document.getElementById("server-version").innerText = data.version || "—";

        // Обновляем строку подключения
        const connectEl = document.getElementById("connect-address");
        if (connectEl && data.ip && data.port) {
            connectEl.innerText = data.ip + ":" + data.port;
        }
    } catch (e) {
        document.getElementById("status").innerText = "Ошибка при получении статуса";
        document.getElementById("server-ip").innerText = "—";
        document.getElementById("server-port").innerText = "—";
        document.getElementById("server-version").innerText = "—";
    }
}

async function send(path) {
    const token = localStorage.getItem("api_token");
    if (!token) return alert("Введите API Token");

    try {
        const res = await fetch(path, {
            method: 'POST',
            headers: { "X-API-Token": token }
        });

        if (res.status === 401) return kickToAuth();
        if (!res.ok) throw new Error(`Ошибка: ${res.status}`);

        const data = await res.json();
        document.getElementById("status").innerText = "Статус: " + (data.status || "Нет сообщения");
    } catch (e) {
        document.getElementById("status").innerText = "Ошибка при выполнении команды";
    }
}
