#pragma once

#include <unordered_map>
#include <string>
#include <mutex>
#include <chrono>

class AuthService {
public:
    struct Session {
        std::string role;
        std::chrono::steady_clock::time_point created;
    };

    explicit AuthService(const std::string& tokens_file);

    void load_credentials();

    // login → token
    std::string login(const std::string& login, const std::string& password, std::string& out_role);

    void logout(const std::string& token);

    // вернуть роль или ""
    std::string validate(const std::string& token);

private:
    struct Credential {
        std::string password;
        std::string role;
    };

    std::string tokens_file_;

    std::unordered_map<std::string, Credential> credentials_;
    std::mutex credentials_mx_;

    std::unordered_map<std::string, Session> sessions_;
    std::mutex sessions_mx_;

    std::string generate_token();
    void cleanup_sessions();
};