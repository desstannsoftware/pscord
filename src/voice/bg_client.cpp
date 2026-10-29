// ps5-discord - App-side client for the background voice daemon.
// Copyright (C) 2026 Desstann
// SPDX-License-Identifier: MIT

#include "voice/bg_client.hpp"

#include "voice/bg_protocol.hpp"

#include "audio/stream_ring.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace voice::bg
{
namespace
{

bool send_all(int fd, const char *data, size_t size)
{
    size_t sent = 0;
    while (sent < size)
    {
        const ssize_t n = ::send(fd, data + sent, size - sent, 0);
        if (n <= 0)
        {
            return false;
        }
        sent += size_t(n);
    }
    return true;
}

bool send_all(int fd, const std::string &data)
{
    return send_all(fd, data.data(), data.size());
}

// One command, one connection. The daemon closes after the reply line.
bool exchange(const std::string &command, std::string *reply, int timeout_ms)
{
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        return false;
    }
    timeval tv{};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(kPort);
    addr.sin_addr.s_addr = inet_addr(kHost);
    if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0)
    {
        ::close(fd);
        return false;
    }
    const bool sent = send_all(fd, command + "\n");
    std::string out;
    char buf[2048];
    if (sent)
    {
        for (;;)
        {
            const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
            if (n <= 0)
            {
                break;
            }
            out.append(buf, size_t(n));
            if (out.find('\n') != std::string::npos)
            {
                break;
            }
        }
    }
    ::close(fd);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
    {
        out.pop_back();
    }
    if (reply)
    {
        *reply = out;
    }
    return sent && !out.empty();
}

bool send_elf(const std::string &path, std::string *error)
{
    FILE *in = std::fopen(path.c_str(), "rb");
    if (!in)
    {
        *error = "cannot read " + path;
        return false;
    }
    if (std::fseek(in, 0, SEEK_END) != 0)
    {
        std::fclose(in);
        *error = "cannot seek " + path;
        return false;
    }
    const long size = std::ftell(in);
    if (size < 4)
    {
        std::fclose(in);
        *error = "not an ELF: " + path;
        return false;
    }
    if (std::fseek(in, 0, SEEK_SET) != 0)
    {
        std::fclose(in);
        *error = "cannot rewind " + path;
        return false;
    }
    std::vector<char> elf(static_cast<size_t>(size));
    if (std::fread(elf.data(), 1, elf.size(), in) != elf.size())
    {
        std::fclose(in);
        *error = "short read: " + path;
        return false;
    }
    std::fclose(in);
    if (elf[0] != '\x7f')
    {
        *error = "not an ELF: " + path;
        return false;
    }

    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        *error = "socket";
        return false;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(kElfLoaderPort);
    addr.sin_addr.s_addr = inet_addr(kHost);
    if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0)
    {
        ::close(fd);
        *error = "elfldr (port 9021) is not listening";
        return false;
    }
    const bool ok = send_all(fd, elf.data(), elf.size());
    ::close(fd);
    if (!ok)
    {
        *error = "elfldr closed early";
    }
    return ok;
}

Status cached;
std::mutex cached_mu;
std::atomic<bool> watching{false};

void remember(Status next)
{
    std::lock_guard<std::mutex> hold(cached_mu);
    cached = std::move(next);
}

Status parse_status(const std::string &line)
{
    Status status;
    const auto phase = line.find("phase=");
    if (phase == std::string::npos)
    {
        return status;
    }
    const int value = std::atoi(line.c_str() + phase + 6);
    status.phase = value == 1   ? Phase::connecting
                   : value == 2 ? Phase::connected
                   : value == 3 ? Phase::failed
                                : Phase::idle;
    status.mic = line.find("mic=1") != std::string::npos;
    const auto err = line.find("mic_error=");
    if (err != std::string::npos)
    {
        status.mic_error = line.substr(err + 10);
    }
    if (status.phase == Phase::connecting)
    {
        status.text = "Ses kanalına bağlanılıyor...";
    }
    else if (status.phase == Phase::failed)
    {
        status.text = status.mic_error.empty() ? "Ses hatası" : status.mic_error;
    }
    return status;
}

void watch()
{
    if (watching.exchange(true))
    {
        return;
    }
    std::thread(
        []
        {
            for (;;)
            {
                std::string line;
                if (exchange("STATUS", &line, 800))
                {
                    remember(parse_status(line));
                }
                ::usleep(400 * 1000);
            }
        })
        .detach();
}

Status take_live(const Status &local)
{
    std::lock_guard<std::mutex> hold(cached_mu);
    if (cached.phase == Phase::idle)
    {
        return local;
    }
    Status mixed = cached;
    mixed.muted = local.muted;
    return mixed;
}

} // namespace

bool available()
{
    std::string reply;
    return exchange("PING", &reply, 400) && reply == std::string("PONG ") + kVersion;
}

void ensure_party_pump(const std::string &elf_path)
{
    // STATUS reports pump=1 once party-audio-pump has attached. Re-send if the
    // previous injector died after a bg-voice restart.
    std::string status;
    if (exchange("STATUS", &status, 400) && status.find("pump=1") != std::string::npos)
    {
        return;
    }
    static std::mutex send_mu;
    std::lock_guard<std::mutex> hold(send_mu);
    if (exchange("STATUS", &status, 400) && status.find("pump=1") != std::string::npos)
    {
        return;
    }
    std::string error;
    // Prefer /data copy: it is updated over FTP without reinstalling the PKG.
    const char *paths[] = {kPumpElfDataPath, elf_path.c_str(), nullptr};
    for (int i = 0; paths[i]; ++i)
    {
        if (send_elf(paths[i], &error))
        {
            std::printf("[voice] party pump sent (%s)\n", paths[i]);
            return;
        }
    }
    std::printf("[voice] party pump: %s\n", error.c_str());
}

bool ensure_running(const std::string &elf_path, std::string *status)
{
    if (available())
    {
        *status = "background voice already running";
        watch();
        ensure_party_pump(kPumpElfAppPath);
        return true;
    }
    std::string error;
    if (!send_elf(elf_path, &error))
    {
        *status = error;
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (std::chrono::steady_clock::now() < deadline)
    {
        ::usleep(100 * 1000);
        if (available())
        {
            *status = "background voice started";
            watch();
            ensure_party_pump(kPumpElfAppPath);
            return true;
        }
    }
    *status = "background voice was sent to elfldr but never answered";
    return false;
}

bool join(const JoinInfo &info, bool muted, std::string *error)
{
    char cmd[2048];
    const int n =
        std::snprintf(cmd, sizeof(cmd), "JOIN %s %s %s %s %s %s %c", info.endpoint.c_str(),
                      info.token.c_str(), info.session_id.c_str(), info.server_id.c_str(),
                      info.channel_id.c_str(), info.user_id.c_str(), muted ? '1' : '0');
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(cmd))
    {
        *error = "JOIN command too long";
        return false;
    }
    std::string reply;
    if (!exchange(cmd, &reply, 3000) || reply.rfind("OK", 0) != 0)
    {
        *error = reply.empty() ? "no reply" : reply;
        return false;
    }
    Status pending;
    pending.phase = Phase::connecting;
    pending.text = "Ses kanalına bağlanılıyor...";
    pending.muted = muted;
    remember(pending);
    watch();
    return true;
}

bool leave(std::string *error)
{
    std::string reply;
    if (!exchange("LEAVE", &reply, 3000) || reply.rfind("OK", 0) != 0)
    {
        if (error)
        {
            *error = reply.empty() ? "no reply" : reply;
        }
        return false;
    }
    remember(Status{});
    return true;
}

bool set_muted(bool muted, std::string *error)
{
    std::string reply;
    if (!exchange(std::string("MUTE ") + (muted ? "1" : "0"), &reply, 1000) ||
        reply.rfind("OK", 0) != 0)
    {
        if (error)
        {
            *error = reply.empty() ? "no reply" : reply;
        }
        return false;
    }
    return true;
}

std::string status_line()
{
    std::string reply;
    exchange("STATUS", &reply, 1000);
    return reply;
}

Status live(const Status &local)
{
    return take_live(local);
}

void start_relay(hui::audio::StreamRing *ring)
{
    static std::atomic<bool> started{false};
    if (!ring || started.exchange(true))
    {
        return;
    }
    std::thread(
        [ring]
        {
            constexpr size_t kMaxAhead = 48000 / 5; // 200 ms; more means we fell behind
            std::vector<int16_t> pcm(1024 * 2);
            std::vector<float> frames(1024 * 2);
            for (;;)
            {
                const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
                if (fd < 0)
                {
                    ::sleep(1);
                    continue;
                }
                sockaddr_in addr{};
                addr.sin_family = AF_INET;
                addr.sin_port = htons(kPort);
                addr.sin_addr.s_addr = inet_addr(kHost);
                char ok[3] = {};
                if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 ||
                    !send_all(fd, std::string("RELAY\n")) ||
                    ::recv(fd, ok, sizeof(ok), MSG_WAITALL) != 3 || std::memcmp(ok, "OK\n", 3) != 0)
                {
                    ::close(fd);
                    ::sleep(1);
                    continue;
                }
                std::printf("[voice] relay attached\n");
                size_t carry = 0; // bytes of a split frame left from the last read
                auto *bytes = reinterpret_cast<char *>(pcm.data());
                // Mixer stopped (home / other game) → ring stays full. Keep draining
                // the socket (discard) so bg-voice does not see EAGAIN. After ~1.5 s
                // still full, close RELAY so the daemon may use the party pump.
                int full_streak = 0;
                for (;;)
                {
                    if (ring->available() >= kMaxAhead)
                    {
                        if (++full_streak > 300)
                        { // 300 * 5 ms ≈ 1.5 s
                            std::printf("[voice] relay: ring stalled, yielding to party pump\n");
                            break;
                        }
                        char discard[4096];
                        while (::recv(fd, discard, sizeof(discard), MSG_DONTWAIT) > 0)
                        {
                        }
                        ::usleep(5 * 1000);
                        continue;
                    }
                    full_streak = 0;
                    const ssize_t n = ::recv(fd, bytes + carry, pcm.size() * 2 - carry, 0);
                    if (n <= 0)
                    {
                        break;
                    }
                    const size_t total = carry + size_t(n);
                    const size_t count = total / 4;
                    for (size_t i = 0; i < count * 2; ++i)
                    {
                        frames[i] = float(pcm[i]) / 32768.0f;
                    }
                    ring->write(frames.data(), count);
                    carry = total - count * 4;
                    std::memmove(bytes, bytes + count * 4, carry);
                }
                std::printf("[voice] relay lost\n");
                ::close(fd);
                ::sleep(1);
            }
        })
        .detach();
}

} // namespace voice::bg
