#pragma once

#include <string>
//#include "../../includes/json.hpp" - потенциально json команды

class IPCClient {
public:
    static bool send(const std::string& pipe, const std::string& msg, int retries = 10);
};