# MSHost v0.5.0

Панель управления Minecraft-сервером с веб-интерфейсом, CLI и интегрированным HTTPS (Caddy).

## Возможности

- **CLI + Web-панель** — управление сервером из терминала или браузера
- **RCON** — отправка команд, kick/ban игроков, список онлайн
- **HTTP Basic Auth** — логин/пароль с ролями (admin / user)
- **Caddy** — встроенный HTTPS reverse proxy, управляется как child process
- **Горячая перезагрузка** — конфиги перечитываются при старте/рестарте без перезапуска программы
- **Rate limiting** — защита API от спама
- **Скачивание модпака** — с ограничением скорости

---

## Быстрый старт

### Сборка

```bash
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Бинарник: `build/bin/Release/mshost.exe` → скопировать в `bin/mshost.exe`

**Требования:** CMake 3.15+, MSVC (Visual Studio 2017+), Windows SDK

### Запуск

```bash
launch.bat
```

Или напрямую:

```bash
bin\mshost.exe --all        # Запустить MC сервер + Web + Caddy
bin\mshost.exe --web-only   # Только Web + Caddy
bin\mshost.exe --mc-only    # Только MC сервер
bin\mshost.exe              # Ничего не запускать автоматически
```

Файл `launch_args.txt` — аргументы по умолчанию (один на строку).

---

## Конфигурация (config.json)

```jsonc
{
  "java": {
    "path": "C:\\...\\java.exe",    // Путь к Java
    "jvm_args": ["-Xmx16G"]        // Аргументы JVM
  },
  "server": {
    "directory": "C:\\...\\Server",  // Папка сервера
    "forge_args": "@libraries/...",  // Forge аргументы
    "user_jvm_args": "@user_jvm_args.txt",
    "stop_timeout_ms": 20000,        // Таймаут graceful stop
    "force_kill_timeout_ms": 5000,   // Таймаут force kill
    "rcon": {
      "enabled": true,
      "host": "127.0.0.1",
      "port": 25575,
      "password": "your_password",
      "retry_interval": 3000,
      "max_retries": 12
    }
  },
  "web": {
    "port": 8080,
    "tokens_file": "tokens",          // Файл учётных данных
    "logs_path": "server.log",
    "modpack_path": "modpack.rar",
    "web_root": "./site",
    "upload_limit": 7,                 // МБ/с для скачивания
    "max_log_lines": 500,
    "rate_limit_ms": 1000,
    "thread_pool_size": 4,
    "server_ip": "your.domain.com",    // Отображается в панели
    "server_port": 25565,
    "server_version": "Forge 1.20.1"
  },
  "caddy": {
    "enabled": true,
    "exe_path": "caddy\\caddy.exe",
    "config_path": "caddy\\Caddyfile",
    "working_dir": "caddy"
  },
  "logging": {
    "console": true,
    "log_level": "INFO"
  }
}
```

---

## Учётные данные (tokens)

Формат: `логин:пароль:роль`

```
admin:strongpassword:admin
viewer:viewerpass:user
```

| Роль | Доступ |
|------|--------|
| `admin` | Полный: start/stop, команды, kick/ban, логи, скачивание, выключение |
| `user` | Только чтение: статус, список игроков |

Перечитать на лету: команда `web-updatecreds`

---

## CLI-команды

| Команда | Описание |
|---------|----------|
| `server-start` | Запустить MC сервер (с перечитыванием конфига) |
| `server-stop` | Остановить MC сервер |
| `server-restart` | Перезапустить MC сервер |
| `server-status` | Показать статус сервера |
| `web-start` | Запустить Web + Caddy |
| `web-stop` | Остановить Web + Caddy |
| `web-restart` | Перезапустить Web + Caddy (с перечитыванием конфигов) |
| `web-updatecreds` | Перечитать файл учётных данных |
| `caddy-start` | Запустить Caddy отдельно |
| `caddy-stop` | Остановить Caddy |
| `caddy-restart` | Перезапустить Caddy |
| `caddy-status` | Показать статус Caddy |
| `exit` | Остановить всё и выйти |
| `help` | Список команд |
| `/команда` | Отправить команду в MC сервер |
| `prank <игрок>` | Психо-пранк игроку |

---

## REST API

Все эндпоинты требуют `Authorization: Basic <base64(login:password)>`.

### GET (чтение)

| Эндпоинт | Роль | Описание |
|-----------|------|----------|
| `/api/status` | any | Статус сервера, IP, порт, версия, роль |
| `/api/players` | any | Список игроков онлайн |
| `/api/logs` | admin | Последние N строк лога |
| `/api/download-modpack` | any | Скачать модпак |

### POST (действия, admin only)

| Эндпоинт | Body | Описание |
|-----------|------|----------|
| `/api/start` | — | Запустить сервер |
| `/api/stop` | — | Остановить сервер |
| `/api/command` | `{"command": "say hi"}` | Отправить команду |
| `/api/kick` | `{"player": "Name", "reason": "..."}` | Кикнуть игрока |
| `/api/ban` | `{"player": "Name", "reason": "..."}` | Забанить игрока |
| `/api/exit` | — | Выключить всю программу |

### Коды ответов

| Код | Значение |
|-----|----------|
| 200 | Успех |
| 401 | Неверные учётные данные |
| 403 | Недостаточно прав |
| 429 | Слишком частые запросы |

---

## Веб-панель

- **auth.html** — страница входа (логин + пароль)
- **index.html** — дашборд:
  - Статус сервера (IP, порт, версия)
  - Кнопки управления (admin): старт, стоп, скачать модпак, выключить ядро
  - Список игроков с kick/ban (admin)
  - Логи сервера с автопрокруткой (admin)
  - Поле ввода команд (admin)
  - Автообновление: статус 2с, логи 5с, игроки 10с

---

## Структура проекта

```
MSHost/
├── src/
|   ├── application.cpp
│   ├── main.cpp                    # Точка входа, CLI
│   ├── minecraft_sv_manager.cpp  # Управление MC сервером
│   ├── caddy_manager.cpp           # Управление Caddy
│   ├── web_sv.cpp              # HTTP API
│   ├── rcon_client.cpp             # RCON клиент
│   └── includes/
│       ├── minecraft_sv_manager.h
│       ├── caddy_manager.h
│       ├── web_sv.h
│       ├── rcon_client.h
│       ├── logger.h
│       ├── httplib.h               # cpp-httplib (header-only)
│       ├── json.hpp                # nlohmann/json (header-only)
│       ├── application.h
│       └── auth_service.h
├── site/                           # Фронтенд
│   ├── index.html
│   ├── auth.html
│   └── assets/
│       ├── js/index.js
│       └── css/index.css
├── caddy/
│   ├── caddy.exe
│   └── Caddyfile
├── bin/mshost.exe                  # Скомпилированный бинарник
├── config.json
├── tokens                          # Учётные данные
├── launch.bat
├── launch_args.txt
└── CMakeLists.txt
```

---

## Лицензия

MIT
