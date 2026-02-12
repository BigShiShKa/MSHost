#pragma once
#include <string>
#include <vector>
#include <cstdint>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

class RCONClient {
public:
    RCONClient() = default;
    ~RCONClient();

    RCONClient(const RCONClient&) = delete;
    RCONClient& operator=(const RCONClient&) = delete;

    bool connect(const std::string& host, int port, const std::string& password);
    void disconnect();
    bool is_connected() const { return connected_; }

    std::string send_command(const std::string& command);

private:
    // RCON packet types
    static constexpr int32_t PACKET_AUTH     = 3;
    static constexpr int32_t PACKET_COMMAND  = 2;
    static constexpr int32_t PACKET_RESPONSE = 0;

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
    int32_t next_request_id_ = 0;

    bool send_packet(int32_t type, const std::string& payload, int32_t request_id);
    Packet receive_packet();
    bool recv_exact(char* buf, int len);
    bool has_valid_socket() const;
    void close_socket();
};
