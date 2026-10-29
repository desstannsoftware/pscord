// ps5-discord - App-side client for the background voice daemon.
// Copyright (C) 2026 Desstann
// SPDX-License-Identifier: MIT
#pragma once

#include "voice/bg_protocol.hpp"
#include "voice/engine.hpp"

#include <string>

namespace voice::bg {

// True when the daemon answers PING with our version.
bool available();

// Send the ELF to elfldr if needed, wait until PING works. *status is human text.
bool ensure_running(const std::string& elf_path, std::string* status);

// Launch party-audio-pump.elf so home-menu / in-game audio reaches the TV.
void ensure_party_pump(const std::string& elf_path);

// Hand a live Discord voice session to the daemon. On success the local Engine
// must not also be started for the same call.
bool join(const JoinInfo& info, bool muted, std::string* error);

bool leave(std::string* error);
bool set_muted(bool muted, std::string* error);

// One-line STATUS reply from the daemon, or empty on failure.
std::string status_line();

// Daemon status when it owns the call, otherwise `local` (the in-app engine).
Status live(const Status& local);

// The daemon's speaker port is not on the TV mix. While the app runs, copy what
// the daemon plays into ring (the app's main-port call stream). Call once.
void start_relay(hui::audio::StreamRing* ring);

} // namespace voice::bg
