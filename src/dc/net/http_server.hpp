// ps5-discord - A small HTTP server for the sign-in page a PC on the same network opens.
// Copyright (C) 2026 Desstann
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <thread>

namespace dc::net
{

struct HttpReply
{
    int status = 200;
    std::string content_type = "application/json; charset=utf-8";
    std::string body;
};

// Called on the server's thread, one request at a time.
using HttpHandler = std::function<HttpReply(const std::string &method, const std::string &path,
                                            const std::string &body)>;

class HttpServer
{
  public:
    // Listens on every interface; false (with error()) when the port cannot be opened.
    bool start(uint16_t port, HttpHandler handler);
    const std::string &error() const
    {
        return error_;
    }

  private:
    void serve();

    int listener_ = -1;
    HttpHandler handler_;
    std::thread thread_;
    std::string error_;
};

// The console's address on the local network, or "" when it cannot be told.
std::string local_ipv4();

} // namespace dc::net
