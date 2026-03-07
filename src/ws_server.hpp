// ws_server.hpp — Minimal WebSocket server (no external deps)
// Supports: upgrade handshake, text frame broadcast
// Works on: Windows (Winsock2) and Linux (POSIX)
#pragma once

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")
   typedef SOCKET sock_t;
#  define SOCK_INVALID INVALID_SOCKET
#  define SOCK_ERR     SOCKET_ERROR
#  define sock_close   closesocket
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
   typedef int sock_t;
#  define SOCK_INVALID (-1)
#  define SOCK_ERR     (-1)
#  define sock_close   close
#endif

#include <thread>
#include <mutex>
#include <set>
#include <string>
#include <vector>
#include <sstream>
#include <functional>
#include <algorithm>
#include <cstring>
#include <cstdint>

// ---- SHA-1 (for WebSocket handshake) --------------------------------
namespace ws_sha1 {
    static uint32_t rotl(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }
    static std::string compute(const std::string& msg) {
        uint32_t h[5] = {0x67452301,0xEFCDAB89,0x98BADCFE,0x10325476,0xC3D2E1F0};
        std::string data = msg;
        size_t orig_len = data.size();
        data += (char)0x80;
        while (data.size() % 64 != 56) data += (char)0x00;
        uint64_t bits = (uint64_t)orig_len * 8;
        for (int i = 7; i >= 0; i--)
            data += (char)((bits >> (i * 8)) & 0xFF);
        for (size_t ch = 0; ch < data.size(); ch += 64) {
            uint32_t w[80];
            for (int i = 0; i < 16; i++)
                w[i] = ((uint8_t)data[ch+i*4]<<24)|((uint8_t)data[ch+i*4+1]<<16)|
                        ((uint8_t)data[ch+i*4+2]<<8)|(uint8_t)data[ch+i*4+3];
            for (int i = 16; i < 80; i++) w[i] = rotl(w[i-3]^w[i-8]^w[i-14]^w[i-16],1);
            uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f,k,t;
            for (int i = 0; i < 80; i++) {
                if      (i<20){f=(b&c)|((~b)&d);k=0x5A827999;}
                else if (i<40){f=b^c^d;          k=0x6ED9EBA1;}
                else if (i<60){f=(b&c)|(b&d)|(c&d);k=0x8F1BBCDC;}
                else          {f=b^c^d;          k=0xCA62C1D6;}
                t=rotl(a,5)+f+e+k+w[i]; e=d; d=c; c=rotl(b,30); b=a; a=t;
            }
            h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e;
        }
        char buf[41];
        std::snprintf(buf, sizeof(buf), "%08x%08x%08x%08x%08x", h[0],h[1],h[2],h[3],h[4]);
        std::string raw;
        for (int i = 0; i < 40; i += 2) {
            uint8_t byte = (uint8_t)std::stoi(std::string(buf+i, 2), nullptr, 16);
            raw += (char)byte;
        }
        return raw;
    }
}

// ---- Base64 encode -----------------------------------------------
namespace ws_b64 {
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    static std::string encode(const std::string& in) {
        std::string out;
        int val=0, valb=-6;
        for (uint8_t c : in) {
            val = (val<<8)+c; valb += 8;
            while (valb >= 0) { out += tbl[(val>>valb)&0x3F]; valb -= 6; }
        }
        if (valb > -6) out += tbl[((val<<8)>>(valb+8))&0x3F];
        while (out.size()%4) out += '=';
        return out;
    }
}

// ---- WebSocket connection ----------------------------------------
struct WsConn {
    sock_t fd;
    std::mutex send_mutex;
    WsConn(sock_t f) : fd(f) {}
    ~WsConn() { sock_close(fd); }
    bool send_text(const std::string& payload) {
        std::lock_guard<std::mutex> lk(send_mutex);
        std::vector<uint8_t> frame;
        frame.push_back(0x81);
        size_t len = payload.size();
        if (len <= 125) {
            frame.push_back((uint8_t)len);
        } else if (len <= 65535) {
            frame.push_back(126);
            frame.push_back((len >> 8) & 0xFF);
            frame.push_back(len & 0xFF);
        } else {
            frame.push_back(127);
            for (int i = 7; i >= 0; i--) frame.push_back((len >> (i*8)) & 0xFF);
        }
        for (char c : payload) frame.push_back((uint8_t)c);
#ifdef _WIN32
        return send(fd, (const char*)frame.data(), (int)frame.size(), 0) >= 0;
#else
        return ::send(fd, (const char*)frame.data(), frame.size(), MSG_NOSIGNAL) >= 0;
#endif
    }
};

// ---- Minimal WebSocket Server ------------------------------------
class WsServer {
public:
    std::mutex clients_mutex;
    std::set<std::shared_ptr<WsConn>> clients;
    std::function<void(std::shared_ptr<WsConn>, const std::string&)> on_message;

    WsServer() {
#ifdef _WIN32
        WSADATA wsa;
        WSAStartup(MAKEWORD(2,2), &wsa);
#endif
    }

    void start(int port) {
        std::thread([this, port]() { accept_loop(port); }).detach();
    }

    void broadcast(const std::string& text) {
        std::set<std::shared_ptr<WsConn>> snapshot;
        {
            std::lock_guard<std::mutex> lk(clients_mutex);
            snapshot = clients;
        }
        std::vector<std::shared_ptr<WsConn>> dead;
        for (auto& c : snapshot) {
            if (!c->send_text(text)) dead.push_back(c);
        }
        if (!dead.empty()) {
            std::lock_guard<std::mutex> lk(clients_mutex);
            for (auto& c : dead) clients.erase(c);
        }
    }

    bool is_active() {
        std::lock_guard<std::mutex> lk(clients_mutex);
        return !clients.empty();
    }

private:
    static std::string trim(const std::string& s) {
        size_t a = s.find_first_not_of(" \t\r\n");
        size_t b = s.find_last_not_of(" \t\r\n");
        if (a == std::string::npos) return "";
        return s.substr(a, b - a + 1);
    }

    static std::string header_value(const std::string& req, const std::string& key) {
        std::istringstream ss(req);
        std::string line;
        while (std::getline(ss, line)) {
            std::string lo = line;
            std::transform(lo.begin(), lo.end(), lo.begin(), ::tolower);
            std::string klo = key;
            std::transform(klo.begin(), klo.end(), klo.begin(), ::tolower);
            if (lo.find(klo) == 0) {
                auto colon = line.find(':');
                if (colon != std::string::npos)
                    return trim(line.substr(colon + 1));
            }
        }
        return "";
    }

    bool do_handshake(sock_t client_fd) {
        char buf[4096] = {};
#ifdef _WIN32
        int n = recv(client_fd, buf, sizeof(buf)-1, 0);
#else
        int n = ::recv(client_fd, buf, sizeof(buf)-1, 0);
#endif
        if (n <= 0) return false;
        std::string req(buf, n);
        std::string ws_key = header_value(req, "Sec-WebSocket-Key");
        if (ws_key.empty()) return false;
        std::string accept_raw = ws_sha1::compute(ws_key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
        std::string accept_b64 = ws_b64::encode(accept_raw);
        std::string response =
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: " + accept_b64 + "\r\n\r\n";
#ifdef _WIN32
        return send(client_fd, response.c_str(), (int)response.size(), 0) > 0;
#else
        return ::send(client_fd, response.c_str(), response.size(), 0) > 0;
#endif
    }

    void accept_loop(int port) {
        sock_t server_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd == SOCK_INVALID) return;
        int opt = 1;
#ifdef _WIN32
        setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
#else
        setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons((uint16_t)port);
        if (bind(server_fd, (sockaddr*)&addr, sizeof(addr)) < 0) { sock_close(server_fd); return; }
        if (listen(server_fd, 10) < 0) { sock_close(server_fd); return; }
        while (true) {
            sock_t client_fd = accept(server_fd, nullptr, nullptr);
            if (client_fd == SOCK_INVALID) continue;
            std::thread([this, client_fd]() {
                if (!do_handshake(client_fd)) { sock_close(client_fd); return; }
                auto conn = std::make_shared<WsConn>(client_fd);
                {
                    std::lock_guard<std::mutex> lk(clients_mutex);
                    clients.insert(conn);
                }
                
                std::vector<uint8_t> buffer;
                char buf[4096];
                while (true) {
#ifdef _WIN32
                    int n = recv(client_fd, buf, sizeof(buf), 0);
#else
                    int n = ::recv(client_fd, buf, sizeof(buf), 0);
#endif
                    if (n <= 0) break;
                    buffer.insert(buffer.end(), buf, buf + n);
                    
                    while (buffer.size() >= 2) {
                        uint8_t b0 = buffer[0];
                        uint8_t b1 = buffer[1];
                        int opcode = b0 & 0x0F;
                        bool masked = (b1 & 0x80) != 0;
                        uint64_t payload_len = b1 & 0x7F;
                        size_t header_len = 2;
                        
                        if (payload_len == 126) {
                            if (buffer.size() < 4) break;
                            payload_len = (buffer[2] << 8) | buffer[3];
                            header_len = 4;
                        } else if (payload_len == 127) {
                            if (buffer.size() < 10) break;
                            payload_len = 0;
                            for (int i=0; i<8; i++) payload_len = (payload_len << 8) | buffer[2+i];
                            header_len = 10;
                        }
                        
                        uint8_t masking_key[4] = {0};
                        if (masked) {
                            if (buffer.size() < header_len + 4) break;
                            for (int i=0; i<4; i++) masking_key[i] = buffer[header_len + i];
                            header_len += 4;
                        }
                        
                        if (buffer.size() < header_len + payload_len) break;
                        
                        if (opcode == 1) { // Text
                            std::string payload;
                            for (size_t i = 0; i < payload_len; i++) {
                                payload += (char)(buffer[header_len + i] ^ (masked ? masking_key[i % 4] : 0));
                            }
                            if (on_message) on_message(conn, payload);
                        } else if (opcode == 8) { // Close
                            goto client_disconnect;
                        }
                        
                        buffer.erase(buffer.begin(), buffer.begin() + header_len + payload_len);
                    }
                }
            client_disconnect:
                std::lock_guard<std::mutex> lk(clients_mutex);
                clients.erase(conn);
            }).detach();
        }
    }
};

// ---- UDP Discovery Broadcaster -----------------------------------
class UdpBroadcaster {
public:
    UdpBroadcaster() {
#ifdef _WIN32
        WSADATA wsa;
        WSAStartup(MAKEWORD(2,2), &wsa);
#endif
    }
    void start(int port, const std::string& message = "BUILDMON_DISCOVERY") {
        std::thread([this, port, message]() {
            sock_t fd = socket(AF_INET, SOCK_DGRAM, 0);
            if (fd == SOCK_INVALID) return;
            int b = 1;
#ifdef _WIN32
            setsockopt(fd, SOL_SOCKET, SO_BROADCAST, (const char*)&b, sizeof(b));
#else
            setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &b, sizeof(b));
#endif
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons((uint16_t)port);
            addr.sin_addr.s_addr = INADDR_BROADCAST;
            while (true) {
#ifdef _WIN32
                sendto(fd, message.c_str(), (int)message.size(), 0, (sockaddr*)&addr, sizeof(addr));
#else
                sendto(fd, message.c_str(), message.size(), 0, (sockaddr*)&addr, sizeof(addr));
#endif
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }).detach();
    }
};
