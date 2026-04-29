#include "ipc_server.h"

IPCServer::IPCServer(std::string pipe_name)
    : pipe_name_(std::move(pipe_name)) {}

void IPCServer::start(std::function<void(const std::string&)> handler) {
    running_ = true;
    thread_ = std::thread(&IPCServer::run, this, handler);
}

void IPCServer::stop() {
    running_ = false;

    // триггерим выход из ConnectNamedPipe через "фейковое" подключение
    HANDLE h = CreateFileA(pipe_name_.c_str(),
                           GENERIC_WRITE,
                           0, nullptr,
                           OPEN_EXISTING,
                           0, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        CloseHandle(h);
    }

    if (thread_.joinable())
        thread_.join();
}

void IPCServer::run(std::function<void(const std::string&)> handler) {

    while (running_) {

        HANDLE pipe = CreateNamedPipeA(
            pipe_name_.c_str(),
            PIPE_ACCESS_INBOUND,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            1,
            0, 0,
            0,
            nullptr
        );

        if (pipe == INVALID_HANDLE_VALUE) {
            Sleep(100);
            continue;
        }

        BOOL connected = ConnectNamedPipe(pipe, nullptr)
                         ? TRUE
                         : (GetLastError() == ERROR_PIPE_CONNECTED);

        if (!connected) {
            CloseHandle(pipe);
            continue;
        }

        char buffer[512];
        DWORD read = 0;

        if (ReadFile(pipe, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
            std::string msg(buffer, read);
            handler(msg);
        }

        FlushFileBuffers(pipe);
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }
}
