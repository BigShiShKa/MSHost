#include "./includes/rcon_client.h"
#include "./includes/logger.h"
#include <cstring>

RCONClient::~RCONClient() {
    disconnect();
}

bool RCONClient::has_valid_socket() const {
#ifdef _WIN32
    return socket_ != INVALID_SOCKET;
#else
    return socket_ >= 0;
#endif
}

void RCONClient::close_socket() {
#ifdef _WIN32
    if (socket_ != INVALID_SOCKET) {
        closesocket(socket_);
        socket_ = INVALID_SOCKET;
    }
#else
    if (socket_ >= 0) {
        ::close(socket_);
        socket_ = -1;
    }
#endif
}

bool RCONClient::connect(const std::string& host, int port, const std::string& password) {
    if (connected_) disconnect();

#ifdef _WIN32
    socket_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_ == INVALID_SOCKET) {
        LOG_ERR("Не удалось создать RCON сокет", "RCON");
        return false;
    }
#else
    socket_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (socket_ < 0) {
        LOG_ERR("Не удалось создать RCON сокет", "RCON");
        return false;
    }
#endif

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(static_cast<u_short>(port));

    if (inet_pton(AF_INET, host.c_str(), &server_addr.sin_addr) <= 0) {
        LOG_ERR("Неверный RCON адрес: " + host, "RCON");
        close_socket();
        return false;
    }

    // Таймаут 5 секунд
#ifdef _WIN32
    DWORD timeout = 5000;
    setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&timeout), sizeof(timeout));
    setsockopt(socket_, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<char*>(&timeout), sizeof(timeout));
#else
    struct timeval timeout;
    timeout.tv_sec = 5;
    timeout.tv_usec = 0;
    setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(socket_, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#endif

    if (::connect(socket_, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)) < 0) {
        LOG_ERR("Не удалось подключиться к RCON: " + host + ":" + std::to_string(port), "RCON");
        close_socket();
        return false;
    }

    // Аутентификация
    int32_t auth_id = ++next_request_id_;
    if (!send_packet(PACKET_AUTH, password, auth_id)) {
        LOG_ERR("RCON: ошибка отправки пакета аутентификации", "RCON");
        close_socket();
        return false;
    }

    Packet response = receive_packet();
    if (response.id == -1) {
        LOG_ERR("RCON: неверный пароль (response_id = -1)", "RCON");
        close_socket();
        return false;
    }
    if (response.id != auth_id) {
        LOG_ERR("RCON: неожиданный response_id: " + std::to_string(response.id) +
                " (ожидался " + std::to_string(auth_id) + ")", "RCON");
        close_socket();
        return false;
    }

    connected_ = true;
    LOG_INFO("RCON подключен: " + host + ":" + std::to_string(port), "RCON");
    return true;
}

void RCONClient::disconnect() {
    close_socket();
    connected_ = false;
}

bool RCONClient::send_packet(int32_t type, const std::string& payload, int32_t request_id) {
    if (!has_valid_socket()) return false;

    // Packet body: request_id(4) + type(4) + payload + \0 + \0
    int32_t body_size = static_cast<int32_t>(4 + 4 + payload.size() + 2);

    std::vector<char> packet(4 + body_size);
    char* p = packet.data();

    std::memcpy(p, &body_size, 4);    p += 4;
    std::memcpy(p, &request_id, 4);   p += 4;
    std::memcpy(p, &type, 4);         p += 4;
    std::memcpy(p, payload.data(), payload.size()); p += payload.size();
    *p++ = '\0';  // payload terminator
    *p++ = '\0';  // padding

    int total = static_cast<int>(packet.size());
    int sent = 0;
    while (sent < total) {
        int n = ::send(socket_, packet.data() + sent, total - sent, 0);
        if (n <= 0) {
            LOG_ERR("RCON: ошибка отправки пакета", "RCON");
            return false;
        }
        sent += n;
    }
    return true;
}

bool RCONClient::recv_exact(char* buf, int len) {
    int received = 0;
    while (received < len) {
        int n = ::recv(socket_, buf + received, len - received, 0);
        if (n <= 0) return false;
        received += n;
    }
    return true;
}

RCONClient::Packet RCONClient::receive_packet() {
    Packet pkt;
    pkt.id = -1;

    if (!has_valid_socket()) return pkt;

    // Читаем размер тела (4 байта)
    int32_t body_size = 0;
    if (!recv_exact(reinterpret_cast<char*>(&body_size), 4)) {
        LOG_ERR("RCON: ошибка чтения размера пакета", "RCON");
        return pkt;
    }

    if (body_size < 10 || body_size > 4110) {
        LOG_WARNING("RCON: некорректный размер пакета: " + std::to_string(body_size), "RCON");
        return pkt;
    }

    // Читаем тело целиком
    std::vector<char> body(body_size);
    if (!recv_exact(body.data(), body_size)) {
        LOG_ERR("RCON: ошибка чтения тела пакета", "RCON");
        return pkt;
    }

    // Парсим: request_id(4) + type(4) + payload + \0 + \0
    std::memcpy(&pkt.id, body.data(), 4);
    std::memcpy(&pkt.type, body.data() + 4, 4);

    int payload_len = body_size - 10;  // -4(id) -4(type) -2(terminators)
    if (payload_len > 0) {
        pkt.payload.assign(body.data() + 8, payload_len);
    }

    return pkt;
}

std::string RCONClient::send_command(const std::string& command) {
    if (!connected_ || !has_valid_socket()) {
        LOG_WARNING("RCON: попытка команды без подключения", "RCON");
        return "";
    }

    int32_t cmd_id = ++next_request_id_;
    if (!send_packet(PACKET_COMMAND, command, cmd_id)) {
        LOG_ERR("RCON: ошибка отправки команды: " + command, "RCON");
        connected_ = false;
        return "";
    }

    Packet response = receive_packet();
    if (response.id == -1) {
        LOG_ERR("RCON: ошибка получения ответа на команду", "RCON");
        connected_ = false;
        return "";
    }

    return response.payload;
}
