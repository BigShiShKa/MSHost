#include "ipc_client.h"
#include <windows.h>

bool IPCClient::send(const std::string& pipe,
                     const std::string& msg,
                     int retries)
{
    for (int i = 0; i < retries; ++i) {

        HANDLE h = CreateFileA(
            pipe.c_str(),
            GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            0,
            nullptr
        );

        if (h != INVALID_HANDLE_VALUE) {

            DWORD written = 0;

            BOOL ok = WriteFile(
                h,
                msg.c_str(),
                static_cast<DWORD>(msg.size()),
                &written,
                nullptr
            );

            CloseHandle(h);
            return ok && written == msg.size();
        }

        Sleep(100);
    }

    return false;
}