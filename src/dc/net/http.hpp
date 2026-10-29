// ps5-discord - Minimal HTTPS/1.1 client on TlsStream.
// Copyright (C) 2026 Desstann
// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace dc::net {

using Headers = std::vector<std::pair<std::string, std::string>>;

struct Response {
    int status = 0; // 0: the request never got an answer
    std::string body;
    std::string error;
};

// One request per connection (Connection: close). url is https://host/path.
Response request(const std::string& method, const std::string& url, const Headers& headers = {},
                 const std::string& body = {});

// GETs url into the file at path, following redirects. progress gets the bytes
// received so far and the total (0 when the server does not say). The body of
// a non-2xx answer is returned in Response::body instead of being written.
Response download(const std::string& url, const std::string& path,
                  const std::function<void(size_t received, size_t total)>& progress = {});

// Splits https://host[:port]/path; false for anything else.
bool split_url(const std::string& url, std::string& host, int& port, std::string& path);

} // namespace dc::net
