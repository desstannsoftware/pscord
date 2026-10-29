// ps5-discord - Mandatory over-the-air updates from desstannsoftware.com.
// Copyright (C) 2026 Desstann
// SPDX-License-Identifier: MIT
//
// update.json names the newest version and its bundle (see tools/release.sh).
// A newer version is downloaded, checked against its SHA-256, written next to
// the installed files and swapped in, then the app restarts into it. An update
// that has been found cannot be skipped; an unreachable manifest lets the app
// start as it is.

#pragma once

#include <cstddef>
#include <mutex>
#include <string>
#include <pthread.h>

namespace update
{

enum class Phase
{
    checking,
    current,
    downloading,
    installing,
    restarting,
    failed
};

struct Status
{
    Phase phase = Phase::checking;
    std::string version; // the version being installed
    std::string notes;
    std::string error;
    size_t received = 0;
    size_t total = 0;
};

class Updater
{
  public:
    static Updater &instance();

    // current is this build's contentVersion; work_dir holds the download.
    void start(const std::string &current, const std::string &manifest_url,
               const std::string &work_dir);
    // Starts again after a failure.
    void retry();

    Status status() const;
    // True while the app must not be used: an update is being checked for
    // (briefly), installed, or failed and waits for a retry.
    bool blocking() const;
    // True once, when the new files are in place and the app should restart.
    bool take_restart();

  private:
    Updater() = default;
    ~Updater();
    void run();
    bool check(std::string &bundle_url, std::string &sha256, size_t &size);
    bool install(const std::string &bundle);
    void set_phase(Phase phase, const std::string &error = {});

    mutable std::mutex lock_;
    Status status_;
    bool found_ = false;
    bool restart_ = false;
    double started_ = 0;
    std::string current_;
    std::string manifest_url_;
    std::string work_dir_;
    pthread_t thread_{};
    bool running_ = false;
};

// Bundle layout, little-endian: "PSCUPD1\n", u32 file count, then per file
// u16 path length, the path relative to the install folder, u64 size, bytes.
constexpr char kBundleMagic[8] = {'P', 'S', 'C', 'U', 'P', 'D', '1', '\n'};

// True when a ("NN.NNN.NNN") is newer than b; false for malformed versions.
bool is_newer(const std::string &a, const std::string &b);

} // namespace update
