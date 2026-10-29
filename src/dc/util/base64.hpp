// ps5-discord - Base64 in the standard and URL-safe alphabets.
// Copyright (C) 2026 Desstann
// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace dc::util
{

std::string base64(const void *data, size_t len, bool url = false, bool pad = true);
// Accepts both alphabets, with or without padding; false on bad input.
bool unbase64(const std::string &text, std::vector<unsigned char> &out);

} // namespace dc::util
