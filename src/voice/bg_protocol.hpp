// ps5-discord - Control protocol between the PKG app and bg-voice.elf.
// The daemon is a privileged payload that survives launching another game;
// the app only brokers Discord gateway credentials and mute state.
// Copyright (C) 2026 Desstann
// SPDX-License-Identifier: MIT
#pragma once

namespace voice::bg {

constexpr const char* kHost = "127.0.0.1";
constexpr int kPort = 9191;
constexpr int kElfLoaderPort = 9021;
constexpr const char* kVersion = "4";
constexpr const char* kReadyPath = "/data/ps5discord/bg-voice.ready";
constexpr const char* kLogPath = "/data/ps5discord/bg-voice.log";
// Bundled next to the app; ShadowMount maps /app0 from the install folder.
constexpr const char* kElfAppPath = "/app0/payload/bg-voice.elf";
constexpr const char* kElfDataPath = "/data/ps5discord/bg-voice.elf";
constexpr const char* kPumpElfAppPath = "/app0/payload/party-audio-pump.elf";
constexpr const char* kPumpElfDataPath = "/data/ps5discord/party-audio-pump.elf";

} // namespace voice::bg
