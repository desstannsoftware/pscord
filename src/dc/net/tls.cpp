// ps5-discord - TLS connections whose ClientHello the ISP filter cannot read.
// Copyright (C) 2026 Desstann
// SPDX-License-Identifier: MIT
#include "dc/net/tls.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "dc/net/dns.hpp"

namespace dc::net
{
namespace
{

std::string g_ca_file;
std::once_flag g_init;
SSL_CTX *g_ctx = nullptr;
BIO_METHOD *g_bio_method = nullptr;

bool find_sni(const uint8_t *rec, size_t len, size_t &off, size_t &name_len)
{
    size_t pos = 5 + 4 + 2 + 32;
    if (len < pos + 1 || rec[0] != 0x16 || rec[5] != 0x01)
    {
        return false;
    }
    pos += 1 + rec[pos];
    if (pos + 2 > len)
    {
        return false;
    }
    pos += 2 + ((rec[pos] << 8) | rec[pos + 1]);
    if (pos + 1 > len)
    {
        return false;
    }
    pos += 1 + rec[pos];
    if (pos + 2 > len)
    {
        return false;
    }
    size_t end = pos + 2 + ((rec[pos] << 8) | rec[pos + 1]);
    pos += 2;
    while (pos + 4 <= end && pos + 4 <= len)
    {
        unsigned type = (rec[pos] << 8) | rec[pos + 1];
        unsigned size = (rec[pos + 2] << 8) | rec[pos + 3];
        if (type == 0 && pos + 9 <= len)
        {
            name_len = (rec[pos + 7] << 8) | rec[pos + 8];
            off = pos + 9;
            return off + name_len <= len && name_len > 1;
        }
        pos += 4 + size;
    }
    return false;
}

bool send_all(int fd, const uint8_t *data, size_t len)
{
    while (len > 0)
    {
        ssize_t n = send(fd, data, len, 0);
        if (n < 0 && errno == EINTR)
        {
            continue;
        }
        if (n <= 0)
        {
            return false;
        }
        data += n;
        len -= size_t(n);
    }
    return true;
}

// The socket BIO's write: the first ClientHello leaves as two records, the rest as is.
int bio_write(BIO *bio, const char *data, int len)
{
    int fd = static_cast<int>(reinterpret_cast<intptr_t>(BIO_get_data(bio)));
    const auto *bytes = reinterpret_cast<const uint8_t *>(data);
    std::vector<uint8_t> split;
    if (!BIO_get_init(bio) || len <= 0)
    {
        return -1;
    }
    if (BIO_get_shutdown(bio) == 0 && split_client_hello(bytes, size_t(len), split))
    {
        BIO_set_shutdown(bio, 1);
        return send_all(fd, split.data(), split.size()) ? len : -1;
    }
    return send_all(fd, bytes, size_t(len)) ? len : -1;
}

int bio_read(BIO *bio, char *buf, int cap)
{
    int fd = static_cast<int>(reinterpret_cast<intptr_t>(BIO_get_data(bio)));
    BIO_clear_retry_flags(bio);
    for (;;)
    {
        ssize_t n = recv(fd, buf, size_t(cap), 0);
        if (n < 0 && errno == EINTR)
        {
            continue;
        }
        return n < 0 ? -1 : int(n);
    }
}

long bio_ctrl(BIO *, int cmd, long, void *)
{
    return cmd == BIO_CTRL_FLUSH ? 1 : 0;
}

void init_once()
{
    SSL_library_init();
    SSL_load_error_strings();
    g_ctx = SSL_CTX_new(TLS_client_method());
    SSL_CTX_set_min_proto_version(g_ctx, TLS1_2_VERSION);
    SSL_CTX_set_verify(g_ctx, SSL_VERIFY_PEER, nullptr);
    if (g_ca_file.empty() || SSL_CTX_load_verify_locations(g_ctx, g_ca_file.c_str(), nullptr) != 1)
    {
        SSL_CTX_set_default_verify_paths(g_ctx);
    }
    g_bio_method = BIO_meth_new(BIO_get_new_index() | BIO_TYPE_SOURCE_SINK, "dc-split-socket");
    BIO_meth_set_write(g_bio_method, bio_write);
    BIO_meth_set_read(g_bio_method, bio_read);
    BIO_meth_set_ctrl(g_bio_method, bio_ctrl);
}

} // namespace

bool split_client_hello(const uint8_t *rec, size_t len, std::vector<uint8_t> &out)
{
    out.clear();
    size_t sni = 0;
    size_t sni_len = 0;
    if (len < 5 || size_t((rec[3] << 8) | rec[4]) + 5 != len || !find_sni(rec, len, sni, sni_len))
    {
        return false;
    }
    size_t cut = sni + sni_len / 2 - 5;
    size_t rest = len - 5 - cut;
    out.reserve(len + 5);
    out.insert(out.end(), rec, rec + 3);
    out.push_back(uint8_t(cut >> 8));
    out.push_back(uint8_t(cut & 0xff));
    out.insert(out.end(), rec + 5, rec + 5 + cut);
    out.insert(out.end(), rec, rec + 3);
    out.push_back(uint8_t(rest >> 8));
    out.push_back(uint8_t(rest & 0xff));
    out.insert(out.end(), rec + 5 + cut, rec + len);
    return true;
}

void set_ca_file(const std::string &path)
{
    g_ca_file = path;
    uint8_t seed[64];
    arc4random_buf(seed, sizeof(seed));
    RAND_add(seed, sizeof(seed), double(sizeof(seed)));
}

TlsStream::~TlsStream()
{
    close();
}

void TlsStream::close()
{
    if (ssl_)
    {
        SSL_shutdown(ssl_);
        SSL_free(ssl_);
        ssl_ = nullptr;
    }
    if (fd_ >= 0)
    {
        ::close(fd_);
        fd_ = -1;
    }
}

bool TlsStream::connect(const std::string &host, uint16_t port, int timeout_ms)
{
    std::call_once(g_init, init_once);
    close();
    in_addr addr{};
    if (!resolve(host, addr))
    {
        error_ = "dns: " + host;
        return false;
    }
    fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0)
    {
        error_ = "socket";
        return false;
    }
    int one = 1;
    setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#ifdef SO_NOSIGPIPE
    setsockopt(fd_, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(port);
    to.sin_addr = addr;

    int flags = fcntl(fd_, F_GETFL, 0);
    fcntl(fd_, F_SETFL, flags | O_NONBLOCK);
    int rc = ::connect(fd_, reinterpret_cast<sockaddr *>(&to), sizeof(to));
    if (rc != 0 && errno == EINPROGRESS)
    {
        pollfd p{fd_, POLLOUT, 0};
        int err = 0;
        socklen_t elen = sizeof(err);
        rc = (poll(&p, 1, timeout_ms) == 1 &&
              getsockopt(fd_, SOL_SOCKET, SO_ERROR, &err, &elen) == 0 && err == 0)
                 ? 0
                 : -1;
    }
    fcntl(fd_, F_SETFL, flags);
    if (rc != 0)
    {
        int so = 0;
        socklen_t solen = sizeof(so);
        getsockopt(fd_, SOL_SOCKET, SO_ERROR, &so, &solen);
        char ip[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &addr, ip, sizeof(ip));
        error_ = "connect: " + host + " " + ip + ":" + std::to_string(port) +
                 " so=" + std::to_string(so) + " errno=" + std::to_string(errno);
        close();
        return false;
    }
    set_read_timeout(timeout_ms);
    timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    BIO *bio = BIO_new(g_bio_method);
    BIO_set_data(bio, reinterpret_cast<void *>(intptr_t(fd_)));
    BIO_set_shutdown(bio, 0);
    BIO_set_init(bio, 1);
    ssl_ = SSL_new(g_ctx);
    SSL_set_bio(ssl_, bio, bio);
    SSL_set_tlsext_host_name(ssl_, host.c_str());
    SSL_set1_host(ssl_, host.c_str());
    if (SSL_connect(ssl_) != 1)
    {
        char buf[256];
        ERR_error_string_n(ERR_get_error(), buf, sizeof(buf));
        error_ = "tls: " + host + ": " + buf;
        close();
        return false;
    }
    return true;
}

void TlsStream::set_read_timeout(int timeout_ms)
{
    timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

bool TlsStream::write_all(const void *data, size_t len)
{
    const auto *p = static_cast<const char *>(data);
    while (len > 0)
    {
        int n = SSL_write(ssl_, p, int(len));
        if (n <= 0)
        {
            error_ = "write";
            return false;
        }
        p += n;
        len -= size_t(n);
    }
    return true;
}

long TlsStream::read(void *buf, size_t cap)
{
    int n = SSL_read(ssl_, buf, int(cap));
    if (n > 0)
    {
        return n;
    }
    return SSL_get_error(ssl_, n) == SSL_ERROR_ZERO_RETURN ? 0 : -1;
}

bool TlsStream::wait_readable(int timeout_ms)
{
    if (SSL_pending(ssl_) > 0)
    {
        return true;
    }
    pollfd p{fd_, POLLIN, 0};
    return poll(&p, 1, timeout_ms) == 1;
}

} // namespace dc::net
