// ps5-discord - fetches Discord endpoints through TlsStream to prove the bypass.
// Copyright (C) 2026 Desstann
// SPDX-License-Identifier: MIT
#include <cstdio>
#include <string>

#include "dc/net/tls.hpp"

int main() {
    const char* hosts[] = {"discord.com", "gateway.discord.gg", "cdn.discordapp.com", "remote-auth-gateway.discord.gg"};
    int failures = 0;
    for (const char* host : hosts) {
        dc::net::TlsStream tls;
        if (!tls.connect(host, 443)) {
            std::printf("%-32s FAIL %s\n", host, tls.error().c_str());
            failures++;
            continue;
        }
        std::string req = std::string("GET / HTTP/1.1\r\nHost: ") + host + "\r\nConnection: close\r\n\r\n";
        tls.write_all(req.data(), req.size());
        char buf[64] = {};
        long n = tls.read(buf, sizeof(buf) - 1);
        std::string line = n > 0 ? std::string(buf, size_t(n)).substr(0, std::string(buf).find('\r')) : "no reply";
        std::printf("%-32s %s\n", host, line.c_str());
    }
    return failures;
}
