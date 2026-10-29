// ps5-discord - WebSocket client (RFC 6455) on TlsStream.
// Copyright (C) 2026 Desstann
// SPDX-License-Identifier: MIT
#include "dc/net/websocket.hpp"

#include <openssl/rand.h>

#include <cstring>

#include "dc/util/base64.hpp"

namespace dc::net {

bool WebSocket::connect(const std::string& url, const Headers& headers) {
    std::string host;
    std::string path;
    int port = 443;
    std::string https = url;
    if (https.compare(0, 6, "wss://") == 0) {
        https = "https://" + https.substr(6);
    }
    if (!split_url(https, host, port, path)) {
        error_ = "bad url";
        return false;
    }
    if (!tls_.connect(host, uint16_t(port))) {
        error_ = tls_.error();
        return false;
    }
    unsigned char nonce[16];
    RAND_bytes(nonce, sizeof(nonce));
    std::string req = "GET " + path + " HTTP/1.1\r\nHost: " + host +
                      "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\n"
                      "Sec-WebSocket-Key: " +
                      util::base64(nonce, sizeof(nonce)) + "\r\n";
    for (const auto& [name, value] : headers) {
        req += name + ": " + value + "\r\n";
    }
    req += "\r\n";
    if (!tls_.write_all(req.data(), req.size())) {
        error_ = "send failed";
        return false;
    }
    std::string head;
    char c = 0;
    while (head.size() < 8192 && head.find("\r\n\r\n") == std::string::npos) {
        if (tls_.read(&c, 1) != 1) {
            error_ = "handshake: no reply";
            return false;
        }
        head.push_back(c);
    }
    if (head.compare(0, 12, "HTTP/1.1 101") != 0) {
        error_ = "handshake: " + head.substr(0, head.find("\r\n"));
        return false;
    }
    close_code_ = 0;
    pending_.clear();
    return true;
}

bool WebSocket::send_frame(int opcode, const std::string& payload) {
    std::string frame;
    frame.push_back(char(0x80 | opcode));
    size_t len = payload.size();
    if (len < 126) {
        frame.push_back(char(0x80 | len));
    } else if (len < 65536) {
        frame.push_back(char(0x80 | 126));
        frame.push_back(char(len >> 8));
        frame.push_back(char(len & 0xff));
    } else {
        frame.push_back(char(0x80 | 127));
        for (int i = 7; i >= 0; i--) {
            frame.push_back(char((uint64_t(len) >> (i * 8)) & 0xff));
        }
    }
    unsigned char mask[4];
    RAND_bytes(mask, sizeof(mask));
    frame.append(reinterpret_cast<char*>(mask), 4);
    size_t start = frame.size();
    frame += payload;
    for (size_t i = 0; i < len; i++) {
        frame[start + i] = char(frame[start + i] ^ mask[i % 4]);
    }
    return tls_.write_all(frame.data(), frame.size());
}

bool WebSocket::send_text(const std::string& text) {
    return tls_.is_open() && send_frame(0x1, text);
}

bool WebSocket::send_binary(const std::string& data) {
    return tls_.is_open() && send_frame(0x2, data);
}

bool WebSocket::read_exact(void* buf, size_t len) {
    auto* p = static_cast<char*>(buf);
    while (len > 0) {
        long n = tls_.read(p, len);
        if (n <= 0) {
            return false;
        }
        p += n;
        len -= size_t(n);
    }
    return true;
}

WebSocket::Result WebSocket::receive(std::string& message, int timeout_ms) {
    if (!tls_.is_open()) {
        return Result::closed;
    }
    for (;;) {
        if (!tls_.wait_readable(timeout_ms)) {
            return Result::timeout;
        }
        uint8_t head[2];
        if (!read_exact(head, 2)) {
            error_ = "connection lost";
            close();
            return Result::closed;
        }
        bool fin = head[0] & 0x80;
        int opcode = head[0] & 0x0f;
        uint64_t len = head[1] & 0x7f;
        if (len == 126 || len == 127) {
            uint8_t ext[8];
            size_t n = len == 126 ? 2 : 8;
            if (!read_exact(ext, n)) {
                close();
                return Result::closed;
            }
            len = 0;
            for (size_t i = 0; i < n; i++) {
                len = (len << 8) | ext[i];
            }
        }
        if (len > (64u << 20)) {
            error_ = "frame too large";
            close();
            return Result::closed;
        }
        std::string payload(size_t(len), '\0');
        if (len > 0 && !read_exact(payload.data(), size_t(len))) {
            close();
            return Result::closed;
        }
        if (opcode == 0x9) {
            send_frame(0xA, payload);
            continue;
        }
        if (opcode == 0x8) {
            close_code_ = payload.size() >= 2 ? (uint8_t(payload[0]) << 8) | uint8_t(payload[1]) : 1005;
            error_ = "closed " + std::to_string(close_code_) + (payload.size() > 2 ? " " + payload.substr(2) : "");
            close();
            return Result::closed;
        }
        if (opcode == 0x1 || opcode == 0x2 || opcode == 0x0) {
            if (opcode != 0x0) {
                pending_binary_ = opcode == 0x2;
            }
            pending_ += payload;
            if (fin) {
                message.swap(pending_);
                pending_.clear();
                binary_ = pending_binary_;
                return Result::message;
            }
        }
        timeout_ms = 15000;
    }
}

void WebSocket::close() {
    tls_.close();
}

} // namespace dc::net
