// ps5-discord - Mandatory over-the-air updates from desstannsoftware.com.
// Copyright (C) 2026 Desstann
// SPDX-License-Identifier: MIT
#include "update/updater.hpp"

#include <openssl/evp.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "dc/net/http.hpp"
#include "third_party/yyjson/yyjson.h"

namespace update {
namespace {

// How long the app waits on a slow manifest before it opens anyway; an update
// found later still takes over the screen.
constexpr double kCheckWaitSeconds = 8.0;
// Where the console keeps this app, as the files are seen from outside the
// sandbox; /app0 is the same folder seen from inside, used when it is writable.
constexpr const char* kInstallRoots[] = {"/data/homebrew/PPSA99190", "/app0"};
constexpr const char* kNewSuffix = ".pscu-new";

double now_seconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

bool valid_version(const std::string& v) {
    if (v.size() != 10 || v[2] != '.' || v[6] != '.') return false;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i != 2 && i != 6 && (v[i] < '0' || v[i] > '9')) return false;
    }
    return true;
}

std::string file_sha256(const std::string& path) {
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) return {};
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
    std::vector<unsigned char> buf(1 << 16);
    for (size_t n; (n = std::fread(buf.data(), 1, buf.size(), file)) > 0;) EVP_DigestUpdate(ctx, buf.data(), n);
    std::fclose(file);
    unsigned char digest[32];
    unsigned int len = 0;
    EVP_DigestFinal_ex(ctx, digest, &len);
    EVP_MD_CTX_free(ctx);
    static const char* kHex = "0123456789abcdef";
    std::string out;
    for (unsigned int i = 0; i < len; ++i) {
        out += kHex[digest[i] >> 4];
        out += kHex[digest[i] & 15];
    }
    return out;
}

bool make_parents(const std::string& path) {
    for (size_t slash = path.find('/', 1); slash != std::string::npos; slash = path.find('/', slash + 1)) {
        const std::string dir = path.substr(0, slash);
        if (mkdir(dir.c_str(), 0777) != 0 && errno != EEXIST) return false;
        chmod(dir.c_str(), 0777);
    }
    return true;
}

bool safe_path(const std::string& path) {
    if (path.empty() || path[0] == '/' || path.find('\\') != std::string::npos) return false;
    size_t start = 0;
    while (start <= path.size()) {
        const size_t end = std::min(path.find('/', start), path.size());
        const std::string part = path.substr(start, end - start);
        if (part.empty() || part == "." || part == "..") return false;
        start = end + 1;
    }
    return true;
}

std::string install_root() {
    for (const char* root : kInstallRoots) {
        const std::string probe = std::string(root) + "/.pscu-probe";
        if (std::FILE* file = std::fopen(probe.c_str(), "wb")) {
            std::fclose(file);
            std::remove(probe.c_str());
            return root;
        }
    }
    return {};
}

template <typename T>
bool read_le(std::FILE* file, T& value) {
    unsigned char bytes[sizeof(T)];
    if (std::fread(bytes, 1, sizeof(T), file) != sizeof(T)) return false;
    value = 0;
    for (size_t i = 0; i < sizeof(T); ++i) value |= T(bytes[i]) << (8 * i);
    return true;
}

} // namespace

bool is_newer(const std::string& a, const std::string& b) {
    return valid_version(a) && valid_version(b) && a > b;
}

Updater& Updater::instance() {
    static Updater updater;
    return updater;
}

Updater::~Updater() {
    if (running_) pthread_join(thread_, nullptr);
}

void Updater::start(const std::string& current, const std::string& manifest_url, const std::string& work_dir) {
    current_ = current;
    manifest_url_ = manifest_url;
    work_dir_ = work_dir;
    retry();
}

void Updater::retry() {
    if (running_) pthread_join(thread_, nullptr);
    {
        std::lock_guard<std::mutex> hold(lock_);
        status_ = Status{};
        started_ = now_seconds();
    }
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 1 << 20); // TLS handshakes run deep
    running_ = pthread_create(
                   &thread_, &attr,
                   [](void* self) -> void* {
                       static_cast<Updater*>(self)->run();
                       return nullptr;
                   },
                   this) == 0;
    pthread_attr_destroy(&attr);
    if (!running_) set_phase(Phase::current);
}

Status Updater::status() const {
    std::lock_guard<std::mutex> hold(lock_);
    return status_;
}

bool Updater::blocking() const {
    std::lock_guard<std::mutex> hold(lock_);
    switch (status_.phase) {
    case Phase::checking:
        return now_seconds() - started_ < kCheckWaitSeconds;
    case Phase::current:
        return false;
    case Phase::failed:
        return found_;
    default:
        return true;
    }
}

bool Updater::take_restart() {
    std::lock_guard<std::mutex> hold(lock_);
    const bool restart = restart_;
    restart_ = false;
    return restart;
}

void Updater::set_phase(Phase phase, const std::string& error) {
    std::lock_guard<std::mutex> hold(lock_);
    status_.phase = phase;
    status_.error = error;
}

void Updater::run() {
    mkdir(work_dir_.c_str(), 0777);
    const std::string bundle = work_dir_ + "/update.bin";
    std::remove(bundle.c_str());

    std::string url;
    std::string sha256;
    size_t size = 0;
    if (!check(url, sha256, size)) return;

    std::printf("[update] downloading %s\n", url.c_str());
    set_phase(Phase::downloading);
    const dc::net::Response res = dc::net::download(url, bundle, [this, size](size_t received, size_t total) {
        std::lock_guard<std::mutex> hold(lock_);
        status_.received = received;
        status_.total = total ? total : size;
    });
    if (!res.error.empty() || res.status != 200) {
        std::printf("[update] download failed status=%d %s\n", res.status, res.error.c_str());
        set_phase(Phase::failed, "İndirme başarısız oldu. İnternet bağlantını kontrol et.");
        return;
    }
    if (file_sha256(bundle) != sha256) {
        std::remove(bundle.c_str());
        std::printf("[update] checksum mismatch\n");
        set_phase(Phase::failed, "İndirilen güncelleme bozuk çıktı.");
        return;
    }

    set_phase(Phase::installing);
    const bool installed = install(bundle);
    std::remove(bundle.c_str());
    if (!installed) return;
    std::printf("[update] installed %s, restarting\n", status().version.c_str());
    std::lock_guard<std::mutex> hold(lock_);
    status_.phase = Phase::restarting;
    restart_ = true;
}

bool Updater::check(std::string& bundle_url, std::string& sha256, size_t& size) {
    const std::string path = work_dir_ + "/update.json";
    const dc::net::Response res = dc::net::download(manifest_url_, path);
    std::string text;
    if (std::FILE* file = std::fopen(path.c_str(), "rb")) {
        char buf[4096];
        for (size_t n; (n = std::fread(buf, 1, sizeof(buf), file)) > 0;) text.append(buf, n);
        std::fclose(file);
        std::remove(path.c_str());
    }
    if (res.status != 200 || text.empty()) {
        std::printf("[update] no manifest (status=%d %s)\n", res.status, res.error.c_str());
        set_phase(Phase::current);
        return false;
    }

    yyjson_doc* doc = yyjson_read(text.data(), text.size(), 0);
    yyjson_val* root = yyjson_doc_get_root(doc);
    const char* version = yyjson_get_str(yyjson_obj_get(root, "version"));
    const char* url = yyjson_get_str(yyjson_obj_get(root, "bundle"));
    const char* hash = yyjson_get_str(yyjson_obj_get(root, "sha256"));
    const char* notes = yyjson_get_str(yyjson_obj_get(root, "notes"));
    size = size_t(yyjson_get_uint(yyjson_obj_get(root, "size")));
    const std::string latest = version ? version : "";
    bundle_url = url ? url : "";
    sha256 = hash ? hash : "";
    const std::string note_text = notes ? notes : "";
    yyjson_doc_free(doc);

    if (!is_newer(latest, current_) || bundle_url.empty() || sha256.size() != 64) {
        std::printf("[update] up to date (installed %s, latest %s)\n", current_.c_str(), latest.c_str());
        set_phase(Phase::current);
        return false;
    }
    std::lock_guard<std::mutex> hold(lock_);
    found_ = true;
    status_.version = latest;
    status_.notes = note_text;
    status_.total = size;
    return true;
}

bool Updater::install(const std::string& bundle) {
    const std::string root = install_root();
    if (root.empty()) {
        std::printf("[update] no writable install folder\n");
        set_phase(Phase::failed, "Uygulama klasörüne yazılamadı.");
        return false;
    }
    std::FILE* in = std::fopen(bundle.c_str(), "rb");
    if (!in) {
        set_phase(Phase::failed, "Güncelleme dosyası açılamadı.");
        return false;
    }
    char magic[sizeof(kBundleMagic)];
    uint32_t count = 0;
    bool ok = std::fread(magic, 1, sizeof(magic), in) == sizeof(magic) &&
              std::memcmp(magic, kBundleMagic, sizeof(magic)) == 0 && read_le(in, count);

    // Every file is written next to its old self first; only a complete set is swapped in.
    std::vector<std::string> written;
    std::vector<char> buf(1 << 16);
    for (uint32_t i = 0; ok && i < count; ++i) {
        uint16_t path_len = 0;
        uint64_t size = 0;
        std::string rel;
        ok = read_le(in, path_len) && path_len > 0;
        if (ok) {
            rel.resize(path_len);
            ok = std::fread(rel.data(), 1, path_len, in) == path_len && safe_path(rel) && read_le(in, size);
        }
        if (!ok) break;
        const std::string target = root + "/" + rel + kNewSuffix;
        std::FILE* out = make_parents(target) ? std::fopen(target.c_str(), "wb") : nullptr;
        ok = out != nullptr;
        if (ok) written.push_back(root + "/" + rel);
        while (ok && size > 0) {
            const size_t chunk = size_t(std::min<uint64_t>(size, buf.size()));
            ok = std::fread(buf.data(), 1, chunk, in) == chunk && std::fwrite(buf.data(), 1, chunk, out) == chunk;
            size -= chunk;
        }
        if (out) ok = std::fclose(out) == 0 && ok;
        if (!ok) std::printf("[update] cannot write %s\n", target.c_str());
    }
    std::fclose(in);

    if (!ok) {
        for (const std::string& path : written) std::remove((path + kNewSuffix).c_str());
        set_phase(Phase::failed, "Güncelleme kurulamadı.");
        return false;
    }
    for (const std::string& path : written) {
        // The running eboot.bin can be replaced: the process keeps its own copy.
        std::remove(path.c_str());
        if (std::rename((path + kNewSuffix).c_str(), path.c_str()) != 0) {
            std::printf("[update] cannot replace %s\n", path.c_str());
            set_phase(Phase::failed, "Güncelleme kurulamadı.");
            return false;
        }
        // The console only starts an app whose files are open to all; fopen
        // would otherwise leave them without the execute bits.
        if (chmod(path.c_str(), 0777) != 0) {
            std::printf("[update] cannot open up %s (errno %d)\n", path.c_str(), errno);
        }
    }
    return true;
}

} // namespace update
