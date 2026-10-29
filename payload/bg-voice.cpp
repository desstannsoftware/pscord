// ps5-discord - Background voice daemon.
// A privileged payload (elfldr :9021). It is not a title, so launching a game
// does not suspend it. The PSCord app hands it the Discord voice session; this
// process owns the microphone, speakers, Opus and DAVE for the rest of the call.
// Copyright (C) 2026 Desstann
// SPDX-License-Identifier: MIT

#include "voice/engine.hpp"

#include "audio/mixer.hpp"
#include "audio/stream_ring.hpp"
#include "dc/net/tls.hpp"
#include "platform/ps5/audio_out.hpp"

#include <ps5/kernel.h>

#include <arpa/inet.h>
#include <dirent.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/user.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

extern "C" {
int sceNetInit(void);
int sceNetPoolCreate(const char* name, int size, int flags);
}

namespace {

constexpr int kPort = 9191;
constexpr const char* kVersion = "4";
constexpr const char* kLogPath = "/data/ps5discord/bg-voice.log";

FILE* g_log = nullptr;
bool g_audio = false;
// The PSCord app, while it runs, plays the call on its own main port: a payload
// side port is not on the TV mix. -1 when nobody listens.
std::atomic<int> g_relay{-1};
std::atomic<int> g_pump{-1}; // PartyDaemon MAIN pump — audible on the home menu / over games
std::atomic<bool> g_quit{false};
hui::ps5::AudioOut* g_audio_dev = nullptr;
bool g_fallback_logged = false;
// Keep feeding the pump ~300 ms after speech so MAIN is not open/close per syllable.
int g_pump_hangover = 0;

void log_line(const char* fmt, ...);

static void set_pump_mode(bool on) {
    if (g_audio_dev) {
        g_audio_dev->set_soft_pace(on);
    }
}

// In-app: RELAY socket connected → only the app plays (never the party pump).
// Menu / other game: app closes RELAY after its ring stalls → only the pump.
// soft_pace only while we are actually feeding audible PCM to the pump —
// enabling it on an idle pump connection fills BGM with paced silence and
// crackles the home menu before PSCord is even opened.
// Never switch on EAGAIN; that oscillated and froze PSCord.
void relay_grain(const int16_t* grain, int frames, void*) {
    auto grain_peak = [&]() {
        int peak = 0;
        const int n = frames * 2;
        for (int i = 0; i < n; ++i) {
            const int a = grain[i] < 0 ? -grain[i] : grain[i];
            if (a > peak)
                peak = a;
        }
        return peak;
    };

    const int fd = g_relay.load(std::memory_order_relaxed);
    if (fd >= 0) {
        if (g_fallback_logged) {
            g_fallback_logged = false;
            set_pump_mode(false);
            log_line("relay: app is playing the call");
        }
        const size_t want = size_t(frames) * 4;
        const ssize_t n = ::send(fd, grain, want, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            int expected = fd;
            if (g_relay.compare_exchange_strong(expected, -1)) {
                ::close(fd);
            }
            g_fallback_logged = true;
            set_pump_mode(false);
            log_line(g_pump.load(std::memory_order_relaxed) >= 0
                         ? "relay: app gone, party pump"
                         : "relay: app gone, no pump yet");
        }
        return;
    }
    const int pump = g_pump.load(std::memory_order_relaxed);
    if (pump < 0) {
        if (g_fallback_logged) {
            set_pump_mode(false);
        }
        return;
    }
    if (!g_fallback_logged) {
        g_fallback_logged = true;
        log_line("relay: app gone, party pump");
    }
    const int peak = grain_peak();
    if (peak > 64) {
        g_pump_hangover = 60; // ~320 ms @ 256/48k
    } else if (g_pump_hangover > 0) {
        --g_pump_hangover;
    } else {
        // Truly idle — no soft_pace, no pump traffic. Menu stays quiet.
        set_pump_mode(false);
        return;
    }
    set_pump_mode(true);
    // Never block the mixer on the pump — ptrace Output is slower than realtime.
    // Dropping here keeps soft_pace on the clock; the pump plays a contiguous
    // thinned stream instead of random catch-up scraps (which sounded like noise).
    const ssize_t n =
        ::send(pump, grain, size_t(frames) * 4, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        int expected = pump;
        if (g_pump.compare_exchange_strong(expected, -1)) {
            ::close(pump);
        }
        set_pump_mode(false);
        g_pump_hangover = 0;
        log_line("pump disconnected errno=%d", errno);
    }
}

void log_line(const char* fmt, ...) {
    if (!g_log) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(g_log, fmt, ap);
    va_end(ap);
    std::fputc('\n', g_log);
    std::fflush(g_log);
    ::fsync(fileno(g_log));
}

// Same offsets as the public payload SDK / ps5debug-NG: clear the per-process
// syscall allow-list so sceAudioIn's syscalls are not rejected.
int unlock_syscall_filter(pid_t pid) {
    const intptr_t kproc = kernel_get_proc(pid);
    intptr_t filter = 0;
    if (!kproc) {
        return -1;
    }
    if (kernel_copyout(kproc + 0x3e8, &filter, sizeof(filter)) < 0 || !filter) {
        return -1;
    }
    unsigned long zero = 0;
    unsigned long all = (unsigned long)-1L;
    if (kernel_copyin(&zero, filter + 0xf0, sizeof(zero)) < 0) {
        return -1;
    }
    if (kernel_copyin(&all, filter + 0xf8, sizeof(all)) < 0) {
        return -1;
    }
    return 0;
}

pid_t find_comm(const char* name) {
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
    size_t size = 0;
    if (sysctl(mib, 4, nullptr, &size, nullptr, 0)) {
        return -1;
    }
    std::vector<char> buf(size);
    if (sysctl(mib, 4, buf.data(), &size, nullptr, 0)) {
        return -1;
    }
    for (char* p = buf.data(); p < buf.data() + size;) {
        auto* ki = reinterpret_cast<struct kinfo_proc*>(p);
        p += ki->ki_structsize ? ki->ki_structsize : sizeof(*ki);
        if (ki->ki_pid > 0 && !std::strcmp(ki->ki_comm, name)) {
            return ki->ki_pid;
        }
    }
    return -1;
}

// Payloads have no title network pool. Without one, loopback accepts but the
// Discord voice websocket and UDP discovery never leave the console.
void init_net() {
    log_line("sceNetInit = %d", sceNetInit());
    log_line("sceNetPool = %d", sceNetPoolCreate("bg-voice", 256 * 1024, 0));
}

pid_t logged_pid() {
    FILE* file = std::fopen(kLogPath, "r");
    if (!file) {
        return -1;
    }
    char line[256] = {};
    pid_t pid = -1;
    while (std::fgets(line, sizeof(line), file)) {
        if (std::strncmp(line, "bg-voice ", 9) != 0) {
            continue;
        }
        if (const char* mark = std::strstr(line, "pid=")) {
            pid = std::atoi(mark + 4);
        }
    }
    std::fclose(file);
    return pid;
}

bool comm_of(pid_t pid, char* out, size_t out_len) {
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
    size_t size = 0;
    if (sysctl(mib, 4, nullptr, &size, nullptr, 0)) {
        return false;
    }
    std::vector<char> buf(size);
    if (sysctl(mib, 4, buf.data(), &size, nullptr, 0)) {
        return false;
    }
    for (char* p = buf.data(); p < buf.data() + size;) {
        auto* ki = reinterpret_cast<struct kinfo_proc*>(p);
        p += ki->ki_structsize ? ki->ki_structsize : sizeof(*ki);
        if (ki->ki_pid == pid) {
            std::snprintf(out, out_len, "%s", ki->ki_comm);
            return true;
        }
    }
    return false;
}

void escalate() {
    const pid_t me = ::getpid();
    log_line("unlock self = %d", unlock_syscall_filter(me));
    // ShellCore's prison can open side audio ports, but it also drops outbound
    // TCP to Discord's voice servers. Keep our own prison so the websocket
    // and UDP discovery can leave the console.
    const pid_t shell = find_comm("SceShellCore");
    if (shell > 0) {
        log_line("unlock shellcore %d = %d", shell, unlock_syscall_filter(shell));
    }
    uint8_t caps[16];
    std::memset(caps, 0xff, sizeof(caps));
    log_line("authid = %d", kernel_set_ucred_authid(me, 0x4800000000010003ULL));
    log_line("caps = %d", kernel_set_ucred_caps(me, caps));
}

std::string find_ca() {
    const char* fixed[] = {"/app0/assets/cacert.pem", "/data/ps5discord/cacert.pem"};
    for (const char* path : fixed) {
        if (::access(path, R_OK) == 0) {
            return path;
        }
    }
    DIR* dir = ::opendir("/data/homebrew");
    if (!dir) {
        return {};
    }
    std::string found;
    while (dirent* ent = ::readdir(dir)) {
        if (ent->d_name[0] == '.') {
            continue;
        }
        std::string path = std::string("/data/homebrew/") + ent->d_name + "/assets/cacert.pem";
        if (::access(path.c_str(), R_OK) == 0) {
            found = path;
            break;
        }
    }
    ::closedir(dir);
    return found;
}

std::vector<std::string> split(const std::string& line) {
    std::vector<std::string> out;
    std::istringstream in(line);
    std::string word;
    while (in >> word) {
        out.push_back(word);
    }
    return out;
}

std::string handle(const std::string& line) {
    const auto parts = split(line);
    if (parts.empty()) {
        return "ERR empty";
    }
    if (parts[0] == "PING") {
        return std::string("PONG ") + kVersion;
    }
    if (parts[0] == "STATUS") {
        const auto st = voice::Engine::instance().status();
        std::ostringstream out;
        out << "OK phase=" << int(st.phase) << " audio=" << (g_audio ? 1 : 0)
            << " mic=" << (st.mic ? 1 : 0) << " pump=" << (g_pump.load() >= 0 ? 1 : 0);
        if (!st.mic_error.empty()) {
            out << " mic_error=" << st.mic_error;
        }
        return out.str();
    }
    if (parts[0] == "LEAVE") {
        voice::Engine::instance().stop();
        log_line("leave");
        return "OK";
    }
    if (parts[0] == "QUIT") {
        // Close the audio ports before exiting. Killing this process while a
        // port is open wedges the system audio server.
        g_quit.store(true);
        log_line("quit");
        return "OK";
    }
    if (parts[0] == "MUTE" && parts.size() == 2) {
        voice::Engine::instance().set_muted(parts[1] == "1");
        return "OK";
    }
    if (parts[0] == "JOIN" && parts.size() == 8) {
        voice::JoinInfo info;
        info.endpoint = parts[1];
        info.token = parts[2];
        info.session_id = parts[3];
        info.server_id = parts[4];
        info.channel_id = parts[5];
        info.user_id = parts[6];
        voice::Engine::instance().start(info);
        voice::Engine::instance().set_muted(parts[7] == "1");
        log_line("join endpoint=%s channel=%s", info.endpoint.c_str(), info.channel_id.c_str());
        return "OK";
    }
    return "ERR unknown";
}

} // namespace

void kill_stragglers(const char* prefix) {
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
    size_t size = 0;
    if (sysctl(mib, 4, nullptr, &size, nullptr, 0)) {
        return;
    }
    std::vector<char> buf(size);
    if (sysctl(mib, 4, buf.data(), &size, nullptr, 0)) {
        return;
    }
    const size_t n = std::strlen(prefix);
    for (char* p = buf.data(); p < buf.data() + size;) {
        auto* ki = reinterpret_cast<struct kinfo_proc*>(p);
        p += ki->ki_structsize ? ki->ki_structsize : sizeof(*ki);
        if (ki->ki_pid <= 1 || ki->ki_pid == ::getpid()) {
            continue;
        }
        if (std::strncmp(ki->ki_comm, prefix, n) == 0) {
            log_line("stopping %s %d = %d", ki->ki_comm, ki->ki_pid, ::kill(ki->ki_pid, SIGKILL));
        }
    }
}

int main() {
    ::mkdir("/data/ps5discord", 0777);
    ::signal(SIGPIPE, SIG_IGN); // the relay peer can vanish mid-send
    const pid_t previous = logged_pid();
    g_log = std::fopen(kLogPath, "a");
    if (!g_log) {
        return 1;
    }
    // Engine progress is printf'd; keep it in the same file as this log.
    ::dup2(fileno(g_log), STDOUT_FILENO);
    ::dup2(fileno(g_log), STDERR_FILENO);
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    log_line("---");
    log_line("bg-voice %s pid=%d", kVersion, ::getpid());
    escalate();
    // Old party pumps hold ptrace on PartyDaemon and make the next attach fail.
    kill_stragglers("party-audio");
    if (previous > 1 && previous != ::getpid()) {
        char comm[32] = {};
        const bool known = comm_of(previous, comm, sizeof(comm));
        if (known && std::strncmp(comm, "Sce", 3) != 0) {
            log_line("stopping previous %d (%s) = %d", previous, comm, ::kill(previous, SIGKILL));
            ::usleep(300 * 1000);
        } else {
            log_line("previous %d not replaced (comm=%s)", previous, known ? comm : "?");
        }
    }
    init_net();

    const std::string ca = find_ca();
    if (ca.empty()) {
        log_line("no cacert.pem; TLS verify will fail");
    } else {
        dc::net::set_ca_file(ca);
        log_line("ca %s", ca.c_str());
    }

    static hui::audio::StreamRing ring(1u << 15);
    static hui::audio::Mixer mixer;
    mixer.attach_stream(0, &ring);
    mixer.prime_stream(0, 1.0f);
    voice::Engine::instance().set_output(&ring);
    static hui::ps5::AudioOut audio;
    g_audio_dev = &audio;
    audio.set_tap(relay_grain, nullptr);
    // BGM (1) then voice (2). The voice port is the party-chat bus and is often
    // busy or silent. Never the main port: the foreground game owns that.
    g_audio = audio.start(mixer, 1, true) || audio.start(mixer, 2, true);
    log_line("sceAudioOut %s", g_audio ? "ok (voice/bgm port)" : "FAILED (no side port)");
    // Background playback goes through party-audio-pump.elf (PartyDaemon MAIN),
    // which is the only path confirmed audible on the home menu.

    const int server = ::socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    ::setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(kPort);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(server, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        // The pid in the log can be stale. The listener is another payload.elf.
        int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
        size_t size = 0;
        if (sysctl(mib, 4, nullptr, &size, nullptr, 0) == 0) {
            std::vector<char> buf(size);
            if (sysctl(mib, 4, buf.data(), &size, nullptr, 0) == 0) {
                for (char* p = buf.data(); p < buf.data() + size;) {
                    auto* ki = reinterpret_cast<struct kinfo_proc*>(p);
                    p += ki->ki_structsize ? ki->ki_structsize : sizeof(*ki);
                    if (ki->ki_pid > 1 && ki->ki_pid != ::getpid() && !std::strcmp(ki->ki_comm, "payload.elf")) {
                        log_line("stopping payload %d = %d", ki->ki_pid, ::kill(ki->ki_pid, SIGKILL));
                    }
                }
            }
        }
        ::usleep(300 * 1000);
        if (::bind(server, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            log_line("port %d already taken; another daemon is running", kPort);
            return 0;
        }
    }
    ::listen(server, 4);
    log_line("listening on 127.0.0.1:%d", kPort);

    for (;;) {
        const int fd = ::accept(server, nullptr, nullptr);
        if (fd < 0) {
            continue;
        }
        std::string line;
        char buf[2048];
        while (line.find('\n') == std::string::npos && line.size() < 8000) {
            const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) {
                break;
            }
            line.append(buf, size_t(n));
        }
        const auto nl = line.find('\n');
        if (nl != std::string::npos) {
            line.resize(nl);
        }
        while (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line == "RELAY") {
            // This socket stays open: every grain the daemon plays is copied
            // to it so the foreground app can put the call on the TV.
            // ~250 ms of PCM. App backpressure fills this on the home menu; too
            // small and normal relay jitter flips us onto the party pump (stutter).
            const int snd = 48 * 1024;
            ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &snd, sizeof snd);
            ::send(fd, "OK\n", 3, 0);
            const int old = g_relay.exchange(fd);
            if (old >= 0) {
                ::close(old);
            }
            // Drop the party pump so it detaches PartyDaemon before/while PSCord
            // finishes launching — holding ptrace across title start can panic.
            const int pump = g_pump.exchange(-1);
            if (pump >= 0) {
                ::close(pump);
                set_pump_mode(false);
                log_line("relay attached — pump dropped for safe launch");
            } else {
                log_line("relay attached");
            }
            continue;
        }
        if (line.rfind("PUMP", 0) == 0) {
            // "PUMP" or "PUMP ScePartyDaemon". VoiceAndAgent etc. accept Output but
            // are silent on the TV — never soft_pace for those or the call vanishes.
            std::string target = "unknown";
            if (line.size() > 5) {
                target = line.substr(5);
                while (!target.empty() && (target.back() == '\r' || target.back() == ' ')) {
                    target.pop_back();
                }
            }
            /* ShellUI is audible but ptracing it wedges home↔game transitions. */
            const bool audible = target == "ScePartyDaemon" || line == "PUMP";
            if (!audible) {
                ::send(fd, "NO\n", 3, 0);
                ::close(fd);
                log_line("party pump rejected target=%s", target.c_str());
                continue;
            }
            // Small buffer + send timeout: ptrace Output is slower than realtime.
            const int snd = 16 * 1024;
            ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &snd, sizeof snd);
            timeval tv{};
            tv.tv_usec = 80 * 1000; // 80 ms
            ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
            ::send(fd, "OK\n", 3, 0);
            const int old = g_pump.exchange(fd);
            if (old >= 0) {
                ::close(old);
            }
            // Do NOT soft_pace on connect — that paced silence crackles the menu
            // before any Discord audio exists. relay_grain enables it on speech.
            if (g_relay.load(std::memory_order_relaxed) < 0) {
                set_pump_mode(false);
            }
            log_line("party pump attached target=%s", target.c_str());
            continue;
        }
        const std::string reply = handle(line) + "\n";
        ::send(fd, reply.data(), reply.size(), 0);
        ::close(fd);
        if (g_quit.load()) {
            break;
        }
    }
    voice::Engine::instance().stop();
    audio.stop();
    log_line("ports closed");
}
