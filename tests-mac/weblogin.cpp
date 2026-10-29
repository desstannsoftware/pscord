// PSCord - Discord session with the PC sign-in page on a Mac.
// Copyright (C) 2026 Desstann
// SPDX-License-Identifier: MIT
#include <chrono>
#include <cstdio>
#include <thread>

#include "dc/client.hpp"

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    dc::Client& client = dc::Client::instance();
    client.start("assets/cacert.pem", "/tmp/dc-session.txt", "/tmp/dc-token.txt");
    client.start_web(8090);
    dc::Snapshot snap;
    for (;;) {
        if (client.snapshot_if_newer(snap.revision, snap) && snap.state == dc::State::ready) {
            std::printf("READY as %s, %zu guilds\n", snap.me.c_str(), snap.guilds.size());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}
