#include "rcon_client.h"

#include <vector>
#include <cstring>

#ifdef _WIN32
#pragma comment(lib, "ws2_32.lib")
#endif

// ─────────────────────────────
// ctor / dtor
// ─────────────────────────────
RCONClient::RCONClient() {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
}

RCONClient::~RCONClient() {
    disconnect();
#ifdef _WIN32
    WSACleanup();
#endif
}

// ─────────────────────────────
// public API
// ─────────────────────────────
bool RCONClient::connect(const std::string& host, int port, const std::string& password) {
    host_ = host;
    port_ = port;
    password_ = password;

    return reconnect();
}

void RCONClient::disconnect() {
    if (!connected_) return;
    close_socket();
    connected_ = false;
}

std::string RCONClient::send_command(const std::string& cmd) {
    if (!ensure_connected()) return "";

    if (!send_packet(2, cmd)) return "";

    auto res = receive_packet();
    return res.payload;
}

// ─────────────────────────────
// internals
// ─────────────────────────────
bool RCONClient::reconnect() {
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

    if (inet_pton(AF_INET, host_.c_str(), &addr.sin_addr) <= 0)
        return false;

    if (::connect(socket_, (sockaddr*)&addr, sizeof(addr)) < 0)
        return false;

    connected_ = true;
    return authenticate();
}

bool RCONClient::ensure_connected() {
    if (connected_) return true;
    return reconnect();
}

bool RCONClient::authenticate() {
    int32_t id = next_id();

    if (!send_packet_raw(id, 3, password_)) {
        connected_ = false;
        return false;
    }

    auto res1 = receive_packet();
    auto res2 = receive_packet();

    if (res2.id == -1) {
        connected_ = false;
        return false;
    }

    return true;
}

int32_t RCONClient::next_id() {
    return ++request_id_;
}

bool RCONClient::send_packet(int32_t type, const std::string& payload) {
    return send_packet_raw(next_id(), type, payload);
}

bool RCONClient::send_packet_raw(int32_t id, int32_t type, const std::string& payload) {
    std::string data = payload + "\0\0";
    int32_t size = 4 + 4 + (int32_t)data.size();

    std::vector<char> buf;
    append(buf, size);
    append(buf, id);
    append(buf, type);
    buf.insert(buf.end(), data.begin(), data.end());

    return send_all(buf.data(), buf.size());
}

bool RCONClient::send_all(const char* data, size_t len) {
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

RCONClient::Packet RCONClient::receive_packet() {
    int32_t size = 0;

    if (!recv_all((char*)&size, 4)) {
        connected_ = false;
        return {};
    }

    std::vector<char> buf(size);

    if (!recv_all(buf.data(), size)) {
        connected_ = false;
        return {};
    }

    Packet res{};
    res.id = *reinterpret_cast<int32_t*>(buf.data());
    res.type = *reinterpret_cast<int32_t*>(buf.data() + 4);

    if (size > 10) {
        res.payload = std::string(buf.data() + 8, size - 10);
    }

    return res;
}

bool RCONClient::recv_all(char* data, size_t len) {
    size_t total = 0;

    while (total < len) {
        int rec = recv(socket_, data + total, (int)(len - total), 0);
        if (rec <= 0) return false;
        total += rec;
    }
    return true;
}

void RCONClient::close_socket() {
#ifdef _WIN32
    closesocket(socket_);
#else
    close(socket_);
#endif
}