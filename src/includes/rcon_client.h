#pragma once
#include <string>
#include <vector>
#include <cstdint>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

class RCONClient {
public:
    RCONClient();
    ~RCONClient();

    RCONClient(const RCONClient&) = delete;
    RCONClient& operator=(const RCONClient&) = delete;

    bool connect(const std::string& host, int port, const std::string& password);
    void disconnect();

    bool is_connected() const { return connected_; }

    std::string send_command(const std::string& command);

private:
    struct Packet {
        int32_t id   = 0;
        int32_t type = 0;
        std::string payload;
    };

#ifdef _WIN32
    SOCKET socket_ = INVALID_SOCKET;
#else
    int socket_ = -1;
#endif

    bool connected_ = false;
    int32_t request_id_ = 0;

    std::string host_;
    std::string password_;
    int port_{25575};

private:
    bool reconnect();
    bool ensure_connected();

    bool authenticate();

    int32_t next_id();

    bool send_packet(int32_t type, const std::string& payload);
    bool send_packet_raw(int32_t id, int32_t type, const std::string& payload);

    Packet receive_packet();

    bool send_all(const char* data, size_t len);
    bool recv_all(char* data, size_t len);

    void close_socket();

    template<typename T>
    void append(std::vector<char>& buf, T value) {
        char* p = reinterpret_cast<char*>(&value);
        buf.insert(buf.end(), p, p + sizeof(T));
    }
};