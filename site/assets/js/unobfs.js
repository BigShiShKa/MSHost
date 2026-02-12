let statusInterval = null;
let alertShown = false;
let lastCommandTime = 0;
let isCommandProcessing = false;
const COMMAND_COOLDOWN = 1000;
let autoScrollEnabled = true;
let isUserScrolling = false;
let scrollTimeout = null;

window.addEventListener("DOMContentLoaded", () => {
    const token = localStorage.getItem("api_token");

    if (!token) {
        window.location.href = "auth.html";
        return;
    }

    // Инициализация кнопки прокрутки
    const scrollBtn = document.querySelector('.scroll-down-btn');
    scrollBtn.addEventListener('click', scrollLogsToBottom);
    
    // Обработчик скролла для логов
    const logBox = document.getElementById('log-box');
    document.querySelector('.log-content-wrapper').addEventListener('scroll', handleLogScroll);

    // Инициализация видимости кнопки
    updateScrollButtonVisibility();

    startStatusLoop();

    // Обработчик Enter
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
    
    // Сбрасываем таймер при каждом скролле
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
    
    // Показываем/скрываем кнопку в зависимости от позиции скролла
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
        
        // Сохраняем текущую позицию скролла и высоту
        const previousScrollTop = logWrapper.scrollTop;
        const previousScrollHeight = logWrapper.scrollHeight;
        const wasScrolledToBottom = previousScrollHeight - previousScrollTop <= logWrapper.clientHeight + 10;
        
        const text = Array.isArray(data.logs) ? data.logs.join("\n") : (data.logs || "Лог пуст");
        
        // Обновляем содержимое только если оно изменилось
        if (logBox.textContent !== text) {
            logBox.textContent = text;
            
            // Восстанавливаем позицию скролла
            if (!wasScrolledToBottom && !isUserScrolling) {
                // Сохраняем относительную позицию скролла
                const newScrollHeight = logWrapper.scrollHeight;
                const heightDifference = newScrollHeight - previousScrollHeight;
                logWrapper.scrollTop = previousScrollTop + heightDifference;
            } else if (wasScrolledToBottom || autoScrollEnabled) {
                // Автоскролл вниз если были внизу или включен автоскролл
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


// Остальные функции остаются без изменений
function startStatusLoop() {
    updateStatus();
    updateLogs();
    statusInterval = setInterval(() => {
        updateStatus();
        updateLogs();
    }, 2000);
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

    const a = document.createElement("a");
    a.href = `/api/download-modpack?token=${encodeURIComponent(token)}`;
    a.download = "";
    a.style.display = "none";
    document.body.appendChild(a);
    a.click();
    document.body.removeChild(a);

    document.getElementById("status").innerText = "Скачиваю модпак...";
}

async function sendCommand() {
    if (isCommandProcessing) return;
    
    const token = localStorage.getItem("api_token");
    const cmdInput = document.getElementById("command");
    const cmd = cmdInput.value.trim();

    if (document.getElementById("status").innerText !== "Статус: Запущен") {
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
            document.getElementById("status").innerText = "Ответ: " + (data.status === "Запущен" ? "Выполнено!" : "Сервер отключен!");
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
        document.getElementById("status").innerText = "Статус: " + (data.status || "Неизвестно");
        document.getElementById("server-ip").innerText = data.ip || "—";
        document.getElementById("server-port").innerText = data.port || "—";
        document.getElementById("server-version").innerText = data.version || "—";
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

        if (path === '/api/exit') {
            document.getElementById("status").innerText = data.message || "Сервер выключается...";
            clearInterval(statusInterval);
            setTimeout(() => {
                document.body.innerHTML = `
                <h1>🛑 Сервер отключён</h1>
                <p>Вы можете закрыть это окно.</p>
                `;
            }, 1000);
            return;
        }

        document.getElementById("status").innerText = "Статус: " + (data.status || "Нет сообщения");
    } catch (e) {
        document.getElementById("status").innerText = "Ошибка при выполнении команды";
    }
}