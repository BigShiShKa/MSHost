#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <stdexcept>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/socket.h>
#endif

class RconClient {
public:
    RconClient() {
#ifdef _WIN32
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    }

    ~RconClient() {
        disconnect();
#ifdef _WIN32
        WSACleanup();
#endif
    }

    bool connect(const std::string& host, int port, const std::string& password) {
        host_ = host;
        port_ = port;
        password_ = password;

        return reconnect();
    }

    void disconnect() {
        if (!connected_) return;

#ifdef _WIN32
        closesocket(socket_);
#else
        close(socket_);
#endif
        connected_ = false;
    }

    bool isConnected() const {
        return connected_;
    }

    std::string command(const std::string& cmd) {
        if (!ensureConnected()) return "";

        if (!sendPacket(2, cmd)) return "";

        auto res = receivePacket();
        return res.payload;
    }

private:
    struct Response {
        int32_t id;
        int32_t type;
        std::string payload;
    };

    bool reconnect() {
        disconnect();

        socket_ = ::socket(AF_INET, SOCK_STREAM, 0);
#ifdef _WIN32
        if (socket_ == INVALID_SOCKET) return false;
#else
        if (socket_ < 0) return false;
#endif

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port_);

        if (inet_pton(AF_INET, host_.c_str(), &addr.sin_addr) <= 0) {
            return false;
        }

        if (::connect(socket_, (sockaddr*)&addr, sizeof(addr)) < 0) {
            return false;
        }

        connected_ = true;

        return authenticate();
    }

    bool ensureConnected() {
        if (connected_) return true;
        return reconnect();
    }

    bool authenticate() {
        int32_t id = nextId();

        if (!sendPacketRaw(id, 3, password_)) {
            connected_ = false;
            return false;
        }

        // RCON может прислать 2 пакета — читаем оба
        auto res1 = receivePacket();
        auto res2 = receivePacket();

        if (res2.id == -1) {
            connected_ = false;
            return false;
        }

        return true;
    }

    int32_t nextId() {
        return ++request_id_;
    }

    bool sendPacket(int type, const std::string& payload) {
        return sendPacketRaw(nextId(), type, payload);
    }

    bool sendPacketRaw(int32_t id, int32_t type, const std::string& payload) {
        std::string data = payload + "\0\0";
        int32_t size = 4 + 4 + data.size();

        std::vector<char> buf;
        append(buf, size);
        append(buf, id);
        append(buf, type);
        buf.insert(buf.end(), data.begin(), data.end());

        return sendAll(buf.data(), buf.size());
    }

    bool sendAll(const char* data, size_t len) {
        size_t total = 0;

        while (total < len) {
            int sent = send(socket_, data + total, (int)(len - total), 0);
            if (sent <= 0) {
                connected_ = false;
                return false;
            }
            total += sent;
        }
        return true;
    }

    Response receivePacket() {
        int32_t size = 0;

        if (!recvAll((char*)&size, 4)) {
            connected_ = false;
            return {};
        }

        std::vector<char> buf(size);

        if (!recvAll(buf.data(), size)) {
            connected_ = false;
            return {};
        }

        Response res{};
        res.id = *reinterpret_cast<int32_t*>(buf.data());
        res.type = *reinterpret_cast<int32_t*>(buf.data() + 4);

        if (size > 10) {
            res.payload = std::string(buf.data() + 8, size - 10);
        }

        return res;
    }

    bool recvAll(char* data, size_t len) {
        size_t total = 0;

        while (total < len) {
            int rec = recv(socket_, data + total, (int)(len - total), 0);
            if (rec <= 0) return false;
            total += rec;
        }
        return true;
    }

    template<typename T>
    void append(std::vector<char>& buf, T value) {
        char* p = reinterpret_cast<char*>(&value);
        buf.insert(buf.end(), p, p + sizeof(T));
    }

private:
    std::string host_;
    std::string password_;
    int port_{25575};

    int32_t request_id_{0};

#ifdef _WIN32
    SOCKET socket_{INVALID_SOCKET};
#else
    int socket_{-1};
#endif

    bool connected_{false};
};