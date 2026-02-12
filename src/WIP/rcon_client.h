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
    RCONClient();
    ~RCONClient();
    
    bool connect(const std::string& host, int port, const std::string& password);
    void disconnect();
    bool is_connected() const { return connected_; }
    
    std::string send_command(const std::string& command);
    bool test_connection();
    
private:
#ifdef _WIN32
    SOCKET socket_ = INVALID_SOCKET;
#else
    int socket_ = -1;
#endif
    bool connected_ = false;
    int request_id_ = 0;
    std::string host_;
    int port_;
    
    bool send_packet(int type, const std::string& payload);
    std::string receive_packet();
    bool authenticate(const std::string& password);
};