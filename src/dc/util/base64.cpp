// ps5-discord - Base64 in the standard and URL-safe alphabets.
// Copyright (C) 2026 Desstann
// SPDX-License-Identifier: MIT
#include "dc/util/base64.hpp"

#include <cstdint>

namespace dc::util
{

std::string base64(const void *data, size_t len, bool url, bool pad)
{
    const char *table = url ? "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"
                            : "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const auto *in = static_cast<const uint8_t *>(data);
    std::string out;
    out.reserve((len + 2) / 3 * 4);
    for (size_t i = 0; i < len; i += 3)
    {
        uint32_t v = uint32_t(in[i]) << 16;
        if (i + 1 < len)
        {
            v |= uint32_t(in[i + 1]) << 8;
        }
        if (i + 2 < len)
        {
            v |= in[i + 2];
        }
        out.push_back(table[(v >> 18) & 63]);
        out.push_back(table[(v >> 12) & 63]);
        if (i + 1 < len)
        {
            out.push_back(table[(v >> 6) & 63]);
        }
        else if (pad)
        {
            out.push_back('=');
        }
        if (i + 2 < len)
        {
            out.push_back(table[v & 63]);
        }
        else if (pad)
        {
            out.push_back('=');
        }
    }
    return out;
}

bool unbase64(const std::string &text, std::vector<unsigned char> &out)
{
    out.clear();
    uint32_t acc = 0;
    int bits = 0;
    for (char c : text)
    {
        int v;
        if (c >= 'A' && c <= 'Z')
        {
            v = c - 'A';
        }
        else if (c >= 'a' && c <= 'z')
        {
            v = c - 'a' + 26;
        }
        else if (c >= '0' && c <= '9')
        {
            v = c - '0' + 52;
        }
        else if (c == '+' || c == '-')
        {
            v = 62;
        }
        else if (c == '/' || c == '_')
        {
            v = 63;
        }
        else if (c == '=')
        {
            break;
        }
        else
        {
            return false;
        }
        acc = (acc << 6) | uint32_t(v);
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            out.push_back(static_cast<unsigned char>((acc >> bits) & 0xff));
        }
    }
    return true;
}

} // namespace dc::util
