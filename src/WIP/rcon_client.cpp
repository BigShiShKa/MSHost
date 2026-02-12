#include "./includes/rcon_client.h"
#include "./includes/Logger.h"
#include <thread>
#include <chrono>

RCONClient::RCONClient() {
#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif
}

RCONClient::~RCONClient() {
    disconnect();
#ifdef _WIN32
    WSACleanup();
#endif
}

bool RCONClient::connect(const std::string& host, int port, const std::string& password) {
    if (connected_) disconnect();
    
    host_ = host;
    port_ = port;
    
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
    server_addr.sin_port = htons(port);
    
    if (inet_pton(AF_INET, host.c_str(), &server_addr.sin_addr) <= 0) {
        LOG_ERR("Неверный RCON адрес: " + host, "RCON");
        disconnect();
        return false;
    }
    
    // Устанавливаем таймаут
#ifdef _WIN32
    DWORD timeout = 5000; // 5 секунд
    setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));
    setsockopt(socket_, SOL_SOCKET, SO_SNDTIMEO, (char*)&timeout, sizeof(timeout));
#else
    struct timeval timeout;
    timeout.tv_sec = 5;
    timeout.tv_usec = 0;
    setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(socket_, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#endif
    
    if (::connect(socket_, (sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        LOG_ERR("Не удалось подключиться к RCON: " + host + ":" + std::to_string(port), "RCON");
        disconnect();
        return false;
    }
    
    // Аутентификация
    if (!authenticate(password)) {
        LOG_ERR("Ошибка аутентификации RCON", "RCON");
        disconnect();
        return false;
    }
    
    connected_ = true;
    LOG_INFO("RCON подключен: " + host + ":" + std::to_string(port), "RCON");
    return true;
}

void RCONClient::disconnect() {
    if (connected_) {
#ifdef _WIN32
        if (socket_ != INVALID_SOCKET) {
            closesocket(socket_);
            socket_ = INVALID_SOCKET;
        }
#else
        if (socket_ >= 0) {
            close(socket_);
            socket_ = -1;
        }
#endif
        connected_ = false;
        LOG_INFO("RCON отключен", "RCON");
    }
}

bool RCONClient::authenticate(const std::string& password) {
    LOG_DEBUG("Начало аутентификации RCON. Длина пароля: " + std::to_string(password.length()), "RCON");
    
    bool result = send_packet(3, password); // 3 = Auth
    
    if (!result) {
        LOG_ERR("Аутентификация RCON не удалась на этапе отправки пакета", "RCON");
        return false;
    }
    
    // Получаем ответ
    std::string response = receive_packet();
    LOG_DEBUG("Ответ аутентификации RCON получен, длина: " + std::to_string(response.length()), "RCON");
    
    // Успешная аутентификация возвращает пустой ответ с определенным request_id
    // Неуспешная возвращает пакет с request_id = -1
    bool auth_success = !response.empty(); // На самом деле логика сложнее
    
    if (auth_success) {
        LOG_INFO("Аутентификация RCON успешна", "RCON");
    } else {
        LOG_ERR("Аутентификация RCON не удалась - неверный пароль или ошибка протокола", "RCON");
    }
    
    return auth_success;
}

bool RCONClient::send_packet(int type, const std::string& payload) {
    if (!connected_) {
        LOG_ERR("Попытка отправить пакет без подключения", "RCON");
        return false;
    }
    
    int32_t request_id = ++request_id_;
    LOG_DEBUG("Отправка RCON пакета. Type: " + std::to_string(type) + 
              ", Request ID: " + std::to_string(request_id) + 
              ", Payload length: " + std::to_string(payload.length()), "RCON");
    
    // Формируем полный payload с нулевыми байтами
    std::string full_payload = payload + std::string("\0\0", 2);
    
    // Рассчитываем размер пакета
    int32_t packet_size = sizeof(int32_t) + sizeof(int32_t) + full_payload.size();
    
    // Создаем буфер для пакета
    std::vector<char> packet_buffer;
    
    // Добавляем размер пакета (4 байта)
    packet_buffer.insert(packet_buffer.end(), 
                        reinterpret_cast<char*>(&packet_size), 
                        reinterpret_cast<char*>(&packet_size) + sizeof(packet_size));
    
    // Добавляем request ID (4 байта)
    packet_buffer.insert(packet_buffer.end(),
                        reinterpret_cast<char*>(&request_id),
                        reinterpret_cast<char*>(&request_id) + sizeof(request_id));
    
    // Добавляем type (4 байта)
    packet_buffer.insert(packet_buffer.end(),
                        reinterpret_cast<char*>(&type),
                        reinterpret_cast<char*>(&type) + sizeof(type));
    
    // Добавляем payload
    packet_buffer.insert(packet_buffer.end(), full_payload.begin(), full_payload.end());
    
    // Отправляем пакет
    int sent = send(socket_, packet_buffer.data(), packet_buffer.size(), 0);
    
    if (sent <= 0) {
        LOG_ERR("Ошибка отправки RCON пакета. Sent: " + std::to_string(sent), "RCON");
        return false;
    }
    
    LOG_DEBUG("RCON пакет отправлен успешно. Bytes: " + std::to_string(sent), "RCON");
    return true;
}

std::string RCONClient::receive_packet() {
    if (!connected_) {
        return "";
    }
    
    // Читаем размер пакета (4 байта)
    int32_t size;
    int received = recv(socket_, reinterpret_cast<char*>(&size), sizeof(size), 0);
    
    if (received <= 0) {
        LOG_ERR("Ошибка чтения размера RCON пакета. Received: " + std::to_string(received), "RCON");
        return "";
    }
    
    LOG_DEBUG("Размер RCON пакета: " + std::to_string(size), "RCON");
    
    if (size <= 0) {
        LOG_WARNING("Некорректный размер RCON пакета: " + std::to_string(size), "RCON");
        return "";
    }
    
    // Читаем остаток пакета
    std::vector<char> buffer(size);
    received = recv(socket_, buffer.data(), size, 0);
    
    if (received <= 0) {
        LOG_ERR("Ошибка чтения тела RCON пакета. Received: " + std::to_string(received), "RCON");
        return "";
    }
    
    LOG_DEBUG("Тело RCON пакета получено. Bytes: " + std::to_string(received), "RCON");
    
    // Парсим пакет
    if (size >= 8) {
        int32_t response_id = *reinterpret_cast<int32_t*>(buffer.data());
        int32_t response_type = *reinterpret_cast<int32_t*>(buffer.data() + 4);
        
        LOG_DEBUG("Response ID: " + std::to_string(response_id) + 
                 ", Response Type: " + std::to_string(response_type), "RCON");
        
        // Для аутентификации: если response_id == -1, то аутентификация не удалась
        if (response_id == -1) {
            LOG_ERR("RCON аутентификация отклонена (response_id = -1)", "RCON");
            return "";
        }
        
        // Возвращаем payload (минус 8 байт заголовка и 2 нулевых байта)
        std::string payload(buffer.data() + 8, size - 8 - 2);
        LOG_DEBUG("Payload получен: '" + payload + "'", "RCON");
        return payload;
    }
    
    return "";
}

std::string RCONClient::send_command(const std::string& command) {
    if (!connected_) {
        LOG_WARNING("Попытка отправить команду через RCON без подключения", "RCON");
        return "";
    }
    
    if (send_packet(2, command)) { // 2 = Command
        return receive_packet();
    }
    
    return "";
}

bool RCONClient::test_connection() {
    if (!connected_) return false;
    
    auto response = send_command("list");
    return !response.empty();
}