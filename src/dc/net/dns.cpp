// ps5-discord - DNS lookups that avoid the ISP's intercepted resolver.
// Copyright (C) 2026 Desstann
// SPDX-License-Identifier: MIT
#include "dc/net/dns.hpp"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <random>

namespace dc::net
{
namespace
{

struct Resolver
{
    const char *ip;
    uint16_t port;
};

constexpr Resolver kResolvers[] = {{"9.9.9.9", 9953}, {"208.67.222.222", 443}};

struct CacheEntry
{
    in_addr addr;
    std::chrono::steady_clock::time_point expires;
};

std::mutex g_lock;
std::map<std::string, CacheEntry> g_cache;

size_t encode_query(uint8_t *out, uint16_t id, const std::string &host)
{
    size_t n = 0;
    out[n++] = id >> 8;
    out[n++] = id & 0xff;
    const uint8_t flags[10] = {0x01, 0, 0, 1, 0, 0, 0, 0, 0, 0};
    std::memcpy(out + n, flags, sizeof(flags));
    n += sizeof(flags);
    size_t start = 0;
    while (start < host.size())
    {
        size_t dot = host.find('.', start);
        size_t len = (dot == std::string::npos ? host.size() : dot) - start;
        if (len == 0 || len > 63 || n + len + 6 > 512)
        {
            return 0;
        }
        out[n++] = static_cast<uint8_t>(len);
        std::memcpy(out + n, host.data() + start, len);
        n += len;
        start += len + 1;
    }
    const uint8_t tail[5] = {0, 0, 1, 0, 1};
    std::memcpy(out + n, tail, sizeof(tail));
    return n + sizeof(tail);
}

size_t skip_name(const uint8_t *msg, size_t len, size_t pos)
{
    while (pos < len)
    {
        if ((msg[pos] & 0xc0) == 0xc0)
        {
            return pos + 2;
        }
        if (msg[pos] == 0)
        {
            return pos + 1;
        }
        pos += msg[pos] + 1;
    }
    return len;
}

bool first_a(const uint8_t *msg, size_t len, uint16_t id, in_addr &out, uint32_t &ttl)
{
    if (len < 12 || ((msg[0] << 8) | msg[1]) != id || (msg[3] & 0x0f) != 0)
    {
        return false;
    }
    unsigned answers = (msg[6] << 8) | msg[7];
    size_t pos = skip_name(msg, len, 12) + 4;
    for (unsigned i = 0; i < answers && pos < len; i++)
    {
        pos = skip_name(msg, len, pos);
        if (pos + 10 > len)
        {
            return false;
        }
        unsigned type = (msg[pos] << 8) | msg[pos + 1];
        ttl = (uint32_t(msg[pos + 4]) << 24) | (msg[pos + 5] << 16) | (msg[pos + 6] << 8) |
              msg[pos + 7];
        unsigned rdlen = (msg[pos + 8] << 8) | msg[pos + 9];
        pos += 10;
        if (type == 1 && rdlen == 4 && pos + 4 <= len)
        {
            std::memcpy(&out, msg + pos, 4);
            return true;
        }
        pos += rdlen;
    }
    return false;
}

} // namespace

bool resolve(const std::string &host, in_addr &out)
{
    if (inet_pton(AF_INET, host.c_str(), &out) == 1)
    {
        return true;
    }
    auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> hold(g_lock);
        auto it = g_cache.find(host);
        if (it != g_cache.end() && it->second.expires > now)
        {
            out = it->second.addr;
            return true;
        }
    }

    static std::mt19937 rng(std::random_device{}());
    uint8_t query[512];
    uint8_t reply[1500];
    uint16_t id = static_cast<uint16_t>(rng());
    size_t qlen = encode_query(query, id, host);
    if (qlen == 0)
    {
        return false;
    }
    for (const Resolver &r : kResolvers)
    {
        int fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0)
        {
            continue;
        }
        timeval tv{2, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_port = htons(r.port);
        inet_pton(AF_INET, r.ip, &to.sin_addr);
        ssize_t n = -1;
        if (connect(fd, reinterpret_cast<sockaddr *>(&to), sizeof(to)) == 0 &&
            send(fd, query, qlen, 0) == ssize_t(qlen))
        {
            n = recv(fd, reply, sizeof(reply), 0);
        }
        close(fd);
        uint32_t ttl = 60;
        if (n > 0 && first_a(reply, size_t(n), id, out, ttl))
        {
            std::lock_guard<std::mutex> hold(g_lock);
            g_cache[host] = {out, now + std::chrono::seconds(ttl < 30 ? 30 : ttl)};
            return true;
        }
    }
    return false;
}

} // namespace dc::net
