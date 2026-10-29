// ps5-discord - WebSocket client (RFC 6455) on TlsStream.
// Copyright (C) 2026 Desstann
// SPDX-License-Identifier: MIT
#pragma once

#include <string>

#include "dc/net/http.hpp"
#include "dc/net/tls.hpp"

namespace dc::net {

class WebSocket {
public:
    enum class Result { message, timeout, closed };

    bool connect(const std::string& url, const Headers& headers = {});
    bool send_text(const std::string& text);
    bool send_binary(const std::string& data);
    // Waits up to timeout_ms for a whole message; answers pings itself.
    Result receive(std::string& message, int timeout_ms);
    // Whether the last message receive() returned was a binary one.
    bool binary() const { return binary_; }
    void close();

    int close_code() const { return close_code_; }
    const std::string& error() const { return error_; }

private:
    bool send_frame(int opcode, const std::string& payload);
    bool read_exact(void* buf, size_t len);

    TlsStream tls_;
    std::string pending_;
    int close_code_ = 0;
    bool binary_ = false;
    bool pending_binary_ = false;
    std::string error_;
};

} // namespace dc::net
