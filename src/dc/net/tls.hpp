// ps5-discord - TLS connections whose ClientHello the ISP filter cannot read.
// Copyright (C) 2026 Desstann
// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

typedef struct ssl_st SSL;

namespace dc::net {

// Splits a TLS ClientHello record into two records cut inside the SNI hostname.
// Returns false (and leaves out empty) when the buffer is not a ClientHello with SNI.
bool split_client_hello(const uint8_t* rec, size_t len, std::vector<uint8_t>& out);

// Where the bundled CA certificates live; empty uses OpenSSL's default paths.
void set_ca_file(const std::string& path);

class TlsStream {
public:
    TlsStream() = default;
    ~TlsStream();
    TlsStream(const TlsStream&) = delete;
    TlsStream& operator=(const TlsStream&) = delete;

    bool connect(const std::string& host, uint16_t port, int timeout_ms = 15000);
    void close();

    bool write_all(const void* data, size_t len);
    // Bytes read, 0 on orderly close, -1 on error or timeout.
    long read(void* buf, size_t cap);
    // True when read() will not block for at least one byte (or the peer closed).
    bool wait_readable(int timeout_ms);
    void set_read_timeout(int timeout_ms);

    bool is_open() const { return ssl_ != nullptr; }
    const std::string& error() const { return error_; }

private:
    int fd_ = -1;
    SSL* ssl_ = nullptr;
    std::string error_;
};

}  // namespace dc::net
