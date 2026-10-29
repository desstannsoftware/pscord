// ps5-discord - Runs dc::Client on a PC: QR in the terminal, then lists guilds.
// Copyright (C) 2026 Desstann
// SPDX-License-Identifier: MIT
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "dc/client.hpp"
#include "third_party/qrcodegen/qrcodegen.h"

static void print_qr(const std::string& text) {
    uint8_t qr[qrcodegen_BUFFER_LEN_MAX];
    uint8_t tmp[qrcodegen_BUFFER_LEN_MAX];
    if (!qrcodegen_encodeText(text.c_str(), tmp, qr, qrcodegen_Ecc_LOW, qrcodegen_VERSION_MIN,
                              qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO, true)) {
        return;
    }
    int size = qrcodegen_getSize(qr);
    for (int y = -2; y < size + 2; y += 2) {
        std::string line;
        for (int x = -2; x < size + 2; x++) {
            bool top = qrcodegen_getModule(qr, x, y);
            bool bottom = qrcodegen_getModule(qr, x, y + 1);
            line += top && bottom ? " " : top ? "\u2584" : bottom ? "\u2580" : "\u2588";
        }
        std::printf("%s\n", line.c_str());
    }
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::string home = std::getenv("HOME") ? std::getenv("HOME") : ".";
    dc::Client& client = dc::Client::instance();
    client.start("", home + "/.ps5discord-session", "");
    dc::Snapshot snap;
    uint64_t rev = 0;
    std::string shown_qr;
    std::string channel = argc > 1 ? argv[1] : "";
    bool opened = false;
    for (int i = 0; i < 6000; i++) {
        if (client.snapshot_if_newer(rev, snap)) {
            rev = snap.revision;
            if (!snap.status.empty()) {
                std::printf("[status] %s%s\n", snap.status.c_str(),
                            snap.pending_user.empty() ? "" : (" (" + snap.pending_user + ")").c_str());
            }
            if (!snap.qr_url.empty() && snap.qr_url != shown_qr) {
                shown_qr = snap.qr_url;
                std::printf("%s\n", snap.qr_url.c_str());
                print_qr(snap.qr_url);
            }
            if (snap.state == dc::State::ready && !opened) {
                std::printf("[ready] %s, %zu entries\n", snap.me.c_str(), snap.guilds.size());
                for (const dc::Guild& g : snap.guilds) {
                    std::printf("  %s (%zu channels)%s%s\n", g.name.c_str(), g.channels.size(),
                                g.channels.empty() ? "" : " first: ",
                                g.channels.empty() ? "" : g.channels[0].name.c_str());
                }
                if (channel.empty() && snap.guilds.size() > 1 && !snap.guilds[1].channels.empty()) {
                    channel = snap.guilds[1].channels[0].id;
                }
                if (!channel.empty()) {
                    client.open_channel(channel);
                }
                opened = true;
            }
            if (opened && !snap.loading_messages && snap.channel_id == channel && !channel.empty()) {
                std::printf("[channel %s] %zu messages %s\n", channel.c_str(), snap.messages.size(),
                            snap.channel_error.c_str());
                for (const dc::Message& m : snap.messages) {
                    std::printf("  %s %s: %s\n", m.time.c_str(), m.author.c_str(), m.content.c_str());
                }
                return 0;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return 1;
}
