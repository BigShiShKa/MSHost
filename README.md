# MSHost

MSHost — это многопроцессная система управления Minecraft-сервером с CLI, веб-интерфейсом и изолированными воркерами. Проект построен вокруг ядра, которое управляет жизненным циклом сервисов через IPC.

## Архитектура

```

MSHost/
├── README.md
├── launch.bat
├── launch_args.txt
├── CMakeLists.txt
│
├── bin/
├── caddy/
├── instances/ # WIP
├── runtime/ # WIP
├── logs/
├── site/
│   ├── assets/
│   │   ├── js/
│   │   │   └── index.js
│   │   ├── css/
│   │   │   └── index.css
│   ├── auth.html
│   ├── favicon.ico / favicon.png
│   └── index.html
│
├── src/
│   ├── main.cpp
│   │
│   ├── core/                  # Ядро (CLI + управление процессами)
│   │   ├── application.cpp
│   │   ├── application.h
│   │   ├── process_manager.cpp
│   │   └── process_manager.h
│   │
│   ├── workers/               # Отдельные процессы
│   │   ├── mc_worker.cpp
│   │   ├── mc_worker.h
│   │   ├── web_worker.cpp
│   │   └── web_worker.h
│   │
│   ├── services/              # Бизнес-логика
│   │   ├── minecraft_sv_manager.cpp
│   │   ├── caddy_manager.cpp
│   │   ├── web_sv.cpp
│   │   ├── rcon_client.cpp
│   │   └── auth_service.cpp
│   │
│   ├── includes/
│   │   ├── minecraft_sv_manager.h
│   │   ├── caddy_manager.h
│   │   ├── web_sv.h
│   │   ├── rcon_client.h
│   │   ├── auth_service.h
│   │   ├── logger.h
│   │   ├── httplib.h
│   │   ├── server_status.h
│   │   └── json.hpp
│   │
│   ├── utils/
│   │   ├── ipc/
│   │   │   ├── ipc_client.cpp
│   │   │   ├── ipc_client.h
│   │   │   ├── ipc_server.cpp
│   │   │   └── ipc_server.h
│   │   ├── console.cpp
│   │   └── console.h

```

---

## Ключевые особенности

### Многопроцессная архитектура
- CORE управляет воркерами через `CreateProcess`
- Каждый worker — отдельный процесс
- Изоляция падений (краш веба не валит ядро)

### IPC (Named Pipes)
- Управление процессами без `CTRL+C`
- Команды передаются через пайпы:
  - `stop`
  - любые команды Minecraft (`/say`, `/kick`, и т.д.)

### Управление процессами
- JobObject → гарантированное убийство дерева процессов
- Graceful shutdown → через IPC
- Force kill → fallback
- Watchdog → авто-рестарт при падении

### CLI интерфейс
- Асинхронный CLI поток
- Расширяемая система команд
- Поддержка проксирования команд в Minecraft:
```

/say hello
/time set day

````

### Логирование
- Потокобезопасный Logger
- Разделение логов:
- основной
- web
- UTF-8 поддержка (Windows console)

---

## Сборка

```bash
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
````

Бинарник:

```
build/bin/Release/mshost.exe
```

---

## Запуск

```bash
bin\mshost.exe --all
bin\mshost.exe --mc-only
bin\mshost.exe --web-only
bin\mshost.exe
```

Аргументы можно задать через:

```
launch_args.txt
```

---

## CLI команды

```
server-start    Запуск Minecraft worker
server-stop     Остановка Minecraft worker
web-start       Запуск Web worker
web-stop        Остановка Web worker
exit            Выход
help            Список команд
```

### Команды Minecraft

Любая строка, начинающаяся с `/`, отправляется в сервер:

```
/say Hello world
/kick Player
/gamemode creative
```

---

## IPC протокол

### Pipe имена

```
\\.\pipe\mc_worker
\\.\pipe\web_worker
```

### Команды

| Команда      | Описание                     |
| ------------ | ---------------------------- |
| `stop`       | graceful shutdown            |
| `say hi`     | команда Minecraft            |
| любые строки | проксируются в stdin сервера |

---

## Жизненный цикл процесса

```
start_process()
    ↓
CreateProcess
    ↓
AssignProcessToJobObject
    ↓
Running

stop_process()
    ↓
IPC "stop"
    ↓
Wait
    ↓
(если не вышел)
force_kill()
```

---

## Watchdog

* Проверка каждые 2 секунды
* Ограничение:

  * максимум 5 рестартов
  * окно 30 секунд
* Экспоненциальная задержка

---

## Конфигурация

Файл:

```
config.json
```

Содержит:

* настройки Java
* путь к серверу
* web настройки
* caddy
* логирование

---

## Web часть

Статика лежит в:

```
site/
```

* `index.html` — дашборд
* `auth.html` — авторизация
* `assets/` — JS/CSS

---

## Caddy

Используется как reverse proxy:

```
caddy/
├── caddy.exe
└── Caddyfile
```

Управляется как отдельный сервис (через web worker).

---

## Типичные проблемы

### Процесс "завис", но считается запущенным

Причина:

* состояние не обновилось

Решение:

* проверять `GetExitCodeProcess`
* использовать watchdog

### Старый код "всплывает"

Классика CMake:

```
rm -rf build
cmake -B build
```

---

## Дальнейшее развитие (ToDo):

* Переход к системе instances/runtime:
 - Ввести Runtime
 - Ввести Instance
 - Ликвидация config.json как god-object
 - Разделение ответсвенности minecraft_sv_manager (god-class)
 - Упростить mc_worker: 
    * получил START
    * получил команду
    * запустил процесс
    * стримит stdout
 - Ввести InstanceManager
 
* auth_service (роли, JWT)
* полноценный REST API
* WebSocket логов
* мониторинг ресурсов (CPU/RAM)
* hot-reload конфигурации
* plugin система

---

## Лицензия
---
MIT
```
