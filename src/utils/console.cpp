#include "console.h"

#ifdef _WIN32
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <iostream>

static std::string wide_to_utf8(const std::wstring& wstr) {
    if (wstr.empty()) return {};

    int size = WideCharToMultiByte(
        CP_UTF8, 0,
        wstr.data(), (int)wstr.size(),
        nullptr, 0, nullptr, nullptr
    );

    std::string result(size, 0);

    WideCharToMultiByte(
        CP_UTF8, 0,
        wstr.data(), (int)wstr.size(),
        result.data(), size,
        nullptr, nullptr
    );

    return result;
}

void setup_console() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    _setmode(_fileno(stdin), _O_U16TEXT);
}

std::string read_line() {
    std::wstring winput;

    if (!std::getline(std::wcin, winput))
        return "__EOF__";

    return wide_to_utf8(winput);
}

#else

#include <iostream>

void setup_console() {}

std::string read_line() {
    std::string input;
    std::getline(std::cin, input);
    return input;
}

#endif