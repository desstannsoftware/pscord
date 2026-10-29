// ps5-discord - Minimal HTTPS/1.1 client on TlsStream.
// Copyright (C) 2026 Desstann
// SPDX-License-Identifier: MIT
#include "dc/net/http.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <strings.h>

#include "dc/net/tls.hpp"

namespace dc::net
{
namespace
{

std::string decode_chunked(const std::string &raw)
{
    std::string out;
    size_t pos = 0;
    while (pos < raw.size())
    {
        size_t eol = raw.find("\r\n", pos);
        if (eol == std::string::npos)
        {
            break;
        }
        size_t len = std::strtoul(raw.c_str() + pos, nullptr, 16);
        pos = eol + 2;
        if (len == 0 || pos + len > raw.size())
        {
            break;
        }
        out.append(raw, pos, len);
        pos += len + 2;
    }
    return out;
}

} // namespace

bool split_url(const std::string &url, std::string &host, int &port, std::string &path)
{
    const std::string scheme = "https://";
    if (url.compare(0, scheme.size(), scheme) != 0)
    {
        return false;
    }
    size_t start = scheme.size();
    size_t slash = url.find('/', start);
    std::string authority =
        url.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
    path = slash == std::string::npos ? "/" : url.substr(slash);
    size_t colon = authority.find(':');
    port = 443;
    if (colon != std::string::npos)
    {
        port = std::atoi(authority.c_str() + colon + 1);
        authority.resize(colon);
    }
    host = authority;
    return !host.empty() && port > 0;
}

Response request(const std::string &method, const std::string &url, const Headers &headers,
                 const std::string &body)
{
    Response res;
    std::string host;
    std::string path;
    int port = 443;
    if (!split_url(url, host, port, path))
    {
        res.error = "bad url";
        return res;
    }
    TlsStream tls;
    if (!tls.connect(host, uint16_t(port)))
    {
        res.error = tls.error();
        return res;
    }
    std::string req = method + " " + path + " HTTP/1.1\r\nHost: " + host +
                      "\r\nConnection: close\r\nAccept-Encoding: identity\r\n";
    for (const auto &[name, value] : headers)
    {
        req += name + ": " + value + "\r\n";
    }
    if (!body.empty() || method == "POST" || method == "PATCH" || method == "PUT")
    {
        req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    }
    req += "\r\n" + body;
    if (!tls.write_all(req.data(), req.size()))
    {
        res.error = "send failed";
        return res;
    }

    std::string raw;
    char buf[16384];
    for (;;)
    {
        long n = tls.read(buf, sizeof(buf));
        if (n <= 0)
        {
            break;
        }
        raw.append(buf, size_t(n));
    }
    size_t head_end = raw.find("\r\n\r\n");
    if (head_end == std::string::npos || raw.compare(0, 5, "HTTP/") != 0)
    {
        res.error = raw.empty() ? "no reply" : "bad reply";
        return res;
    }
    res.status = std::atoi(raw.c_str() + raw.find(' ') + 1);
    std::string head = raw.substr(0, head_end);
    std::string payload = raw.substr(head_end + 4);
    bool chunked = false;
    size_t line = 0;
    while ((line = head.find("\r\n", line)) != std::string::npos)
    {
        line += 2;
        if (strncasecmp(head.c_str() + line, "transfer-encoding:", 18) == 0 &&
            head.find("chunked", line) < head.find("\r\n", line))
        {
            chunked = true;
        }
    }
    res.body = chunked ? decode_chunked(payload) : payload;
    return res;
}

Response download(const std::string &url, const std::string &path,
                  const std::function<void(size_t received, size_t total)> &progress)
{
    Response res;
    std::string target = url;
    for (int hop = 0; hop < 6; ++hop)
    {
        std::string host;
        std::string where;
        int port = 443;
        if (!split_url(target, host, port, where))
        {
            res.error = "bad url";
            return res;
        }
        TlsStream tls;
        if (!tls.connect(host, uint16_t(port)))
        {
            res.error = tls.error();
            return res;
        }
        const std::string req =
            "GET " + where + " HTTP/1.1\r\nHost: " + host +
            "\r\nConnection: close\r\nAccept-Encoding: identity\r\nUser-Agent: PSCord\r\n\r\n";
        if (!tls.write_all(req.data(), req.size()))
        {
            res.error = "send failed";
            return res;
        }

        std::string head;
        std::vector<char> storage(65536);
        char *buf = storage.data();
        size_t head_end = std::string::npos;
        while (head_end == std::string::npos)
        {
            const long n = tls.read(buf, storage.size());
            if (n <= 0)
            {
                res.error = head.empty() ? "no reply" : "bad reply";
                return res;
            }
            head.append(buf, size_t(n));
            head_end = head.find("\r\n\r\n");
            if (head_end == std::string::npos && head.size() > 65536)
            {
                res.error = "bad reply";
                return res;
            }
        }
        if (head.compare(0, 5, "HTTP/") != 0)
        {
            res.error = "bad reply";
            return res;
        }
        std::string body = head.substr(head_end + 4);
        head.resize(head_end);
        res.status = std::atoi(head.c_str() + head.find(' ') + 1);

        std::string location;
        size_t total = 0;
        bool chunked = false;
        for (size_t line = head.find("\r\n"); line != std::string::npos;
             line = head.find("\r\n", line))
        {
            line += 2;
            const size_t end = std::min(head.find("\r\n", line), head.size());
            const char *text = head.c_str() + line;
            const auto value = [&](size_t name_len)
            {
                size_t start = line + name_len;
                while (start < end && head[start] == ' ')
                    ++start;
                return head.substr(start, end - start);
            };
            if (strncasecmp(text, "location:", 9) == 0)
                location = value(9);
            else if (strncasecmp(text, "content-length:", 15) == 0)
                total = std::strtoull(value(15).c_str(), nullptr, 10);
            else if (strncasecmp(text, "transfer-encoding:", 18) == 0)
                chunked = value(18).find("chunked") != std::string::npos;
        }
        if (res.status >= 300 && res.status < 400 && !location.empty())
        {
            target = location;
            continue;
        }
        if (res.status < 200 || res.status >= 300)
        {
            for (long n; body.size() < 65536 && (n = tls.read(buf, storage.size())) > 0;)
                body.append(buf, size_t(n));
            res.body = std::move(body);
            return res;
        }

        std::FILE *file = std::fopen(path.c_str(), "wb");
        if (!file)
        {
            res.error = "cannot write " + path;
            return res;
        }
        // A chunked body is small (GitHub sends assets with a length), so it is decoded once at the
        // end.
        std::string pending = chunked ? body : std::string();
        size_t received = chunked ? 0 : body.size();
        bool ok = chunked || std::fwrite(body.data(), 1, body.size(), file) == body.size();
        if (progress)
            progress(received, total);
        while (ok && (total == 0 || received < total))
        {
            const long n = tls.read(buf, storage.size());
            if (n < 0)
            {
                ok = false;
                res.error = "download interrupted";
                break;
            }
            if (n == 0)
                break;
            if (chunked)
                pending.append(buf, size_t(n));
            else
                ok = std::fwrite(buf, 1, size_t(n), file) == size_t(n);
            received += size_t(n);
            if (progress)
                progress(received, total);
        }
        if (ok && chunked)
        {
            pending = decode_chunked(pending);
            ok = std::fwrite(pending.data(), 1, pending.size(), file) == pending.size();
        }
        ok = std::fclose(file) == 0 && ok;
        if (ok && total != 0 && received < total)
        {
            ok = false;
            res.error = "download incomplete";
        }
        if (!ok)
        {
            if (res.error.empty())
                res.error = "cannot write " + path;
            std::remove(path.c_str());
        }
        return res;
    }
    res.error = "too many redirects";
    return res;
}

} // namespace dc::net
