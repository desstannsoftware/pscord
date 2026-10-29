// ps5-discord - A small HTTP server for the sign-in page a PC on the same network opens.
// Copyright (C) 2026 Desstann
// SPDX-License-Identifier: MIT
#include "dc/net/http_server.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

// No console-tested title has listened on a socket yet. Weak references keep a
// missing export from jumping to address 0: the server then reports it instead.
extern "C"
{
    int bind(int, const struct sockaddr *, socklen_t) __attribute__((weak));
    int listen(int, int) __attribute__((weak));
    int accept(int, struct sockaddr *, socklen_t *) __attribute__((weak));
    int getsockname(int, struct sockaddr *, socklen_t *) __attribute__((weak));
}

namespace dc::net
{
namespace
{

constexpr size_t kMaxRequest = 64 * 1024;

const char *reason(int status)
{
    switch (status)
    {
    case 200:
        return "OK";
    case 400:
        return "Bad Request";
    case 404:
        return "Not Found";
    default:
        return "Error";
    }
}

void send_all(int fd, const std::string &data)
{
    size_t sent = 0;
    while (sent < data.size())
    {
        ssize_t n = send(fd, data.data() + sent, data.size() - sent, 0);
        if (n <= 0)
        {
            return;
        }
        sent += size_t(n);
    }
}

// Reads one request: the request line and, when there is one, a Content-Length body.
bool read_request(int fd, std::string &method, std::string &path, std::string &body)
{
    std::string data;
    char buf[4096];
    size_t header_end = std::string::npos;
    while (header_end == std::string::npos)
    {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0 || data.size() > kMaxRequest)
        {
            return false;
        }
        data.append(buf, size_t(n));
        header_end = data.find("\r\n\r\n");
    }
    size_t line_end = data.find("\r\n");
    std::string line = data.substr(0, line_end);
    size_t a = line.find(' ');
    size_t b = line.find(' ', a + 1);
    if (a == std::string::npos || b == std::string::npos)
    {
        return false;
    }
    method = line.substr(0, a);
    path = line.substr(a + 1, b - a - 1);

    size_t length = 0;
    std::string headers = data.substr(0, header_end);
    for (size_t pos = line_end + 2; pos < headers.size();)
    {
        size_t end = headers.find("\r\n", pos);
        if (end == std::string::npos)
        {
            end = headers.size();
        }
        std::string header = headers.substr(pos, end - pos);
        if (header.size() > 15 && strncasecmp(header.c_str(), "content-length:", 15) == 0)
        {
            length = size_t(std::strtoul(header.c_str() + 15, nullptr, 10));
        }
        pos = end + 2;
    }
    if (length > kMaxRequest)
    {
        return false;
    }
    body = data.substr(header_end + 4);
    while (body.size() < length)
    {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0)
        {
            return false;
        }
        body.append(buf, size_t(n));
    }
    body.resize(length);
    return true;
}

} // namespace

bool HttpServer::start(uint16_t port, HttpHandler handler)
{
    if (!bind || !listen || !accept)
    {
        error_ = "this system does not export bind/listen/accept";
        return false;
    }
    listener_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listener_ < 0)
    {
        error_ = "socket";
        return false;
    }
    int one = 1;
    setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(listener_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 ||
        listen(listener_, 8) != 0)
    {
        error_ = "port " + std::to_string(port) + " is not available";
        close(listener_);
        listener_ = -1;
        return false;
    }
    handler_ = std::move(handler);
    thread_ = std::thread([this] { serve(); });
    thread_.detach();
    return true;
}

void HttpServer::serve()
{
    for (;;)
    {
        int fd = accept(listener_, nullptr, nullptr);
        if (fd < 0)
        {
            continue;
        }
        timeval tv{10, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        std::string method, path, body;
        HttpReply reply;
        if (read_request(fd, method, path, body))
        {
            reply = handler_(method, path, body);
        }
        else
        {
            reply.status = 400;
            reply.body = "{}";
        }
        std::string head = "HTTP/1.1 " + std::to_string(reply.status) + " " + reason(reply.status) +
                           "\r\nContent-Type: " + reply.content_type +
                           "\r\nContent-Length: " + std::to_string(reply.body.size()) +
                           "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
        send_all(fd, head + reply.body);
        close(fd);
    }
}

std::string local_ipv4()
{
    if (!getsockname)
    {
        return {};
    }
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
    {
        return {};
    }
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(53);
    inet_pton(AF_INET, "9.9.9.9", &to.sin_addr);
    std::string ip;
    sockaddr_in me{};
    socklen_t len = sizeof(me);
    if (connect(fd, reinterpret_cast<sockaddr *>(&to), sizeof(to)) == 0 &&
        getsockname(fd, reinterpret_cast<sockaddr *>(&me), &len) == 0)
    {
        char text[INET_ADDRSTRLEN] = {};
        const unsigned char *b = reinterpret_cast<const unsigned char *>(&me.sin_addr);
        std::snprintf(text, sizeof(text), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
        ip = text;
    }
    close(fd);
    return ip == "0.0.0.0" ? std::string() : ip;
}

} // namespace dc::net
