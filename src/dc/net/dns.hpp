// ps5-discord - DNS lookups that avoid the ISP's intercepted resolver.
// Copyright (C) 2026 Desstann
// SPDX-License-Identifier: MIT
#pragma once

#include <netinet/in.h>

#include <string>

namespace dc::net {

// Resolves an IPv4 address with plain DNS on non-standard ports (Quad9 9953,
// OpenDNS 443), which Turkish ISPs neither hijack nor reset. Results are cached.
bool resolve(const std::string& host, in_addr& out);

}  // namespace dc::net
