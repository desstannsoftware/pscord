// ps5-discord - Discord session: QR login, gateway, channels and messages.
// Copyright (C) 2026 Desstann
// SPDX-License-Identifier: MIT
#include "dc/client.hpp"

#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/sha.h>
#include <openssl/x509.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>

#include "dc/net/http.hpp"
#include "dc/net/tls.hpp"
#include "dc/net/websocket.hpp"
#include "dc/markdown.hpp"
#include "i18n/i18n.hpp"
#include "dc/web_page.hpp"
#include "dc/util/base64.hpp"
#include "third_party/yyjson/yyjson.h"
#include "voice/bg_client.hpp"
#include "voice/engine.hpp"

namespace dc
{
namespace
{

using Clock = std::chrono::steady_clock;

constexpr const char *kUserAgent =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/130.0.0.0 Safari/537.36";
constexpr const char *kApi = "https://discord.com/api/v9";
constexpr int kInviteEvery = 3; // launches between invite offers
// Used when config.json cannot be fetched. discord.gg/wfTch3qUa6
constexpr const char *kDefaultInvite = "wfTch3qUa6";

extern "C" int sceKernelSendNotificationRequest(int device, void *request, size_t size,
                                                int blocking);

struct NotifyRequest
{
    char unused[45];
    char message[1024];
};
constexpr size_t kMaxMessages = 100;

std::string json_escape(const std::string &s)
{
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s)
    {
        switch (c)
        {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (c < 0x20)
            {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            }
            else
            {
                out.push_back(char(c));
            }
        }
    }
    return out;
}

std::string properties_json()
{
    return std::string("{\"os\":\"Windows\",\"browser\":\"Chrome\",\"device\":\"\",\"system_"
                       "locale\":\"tr-TR\","
                       "\"browser_user_agent\":\"") +
           kUserAgent +
           "\",\"browser_version\":\"130.0.0.0\",\"os_version\":\"10\",\"referrer\":\"\","
           "\"referring_domain\":\"\",\"release_channel\":\"stable\",\"client_build_number\":"
           "350000,"
           "\"client_event_source\":null}";
}

net::Headers api_headers(const std::string &token)
{
    std::string props = properties_json();
    net::Headers h = {
        {"User-Agent", kUserAgent},
        {"Content-Type", "application/json"},
        {"X-Super-Properties", util::base64(props.data(), props.size())},
        {"X-Discord-Locale", "tr"},
        {"Origin", "https://discord.com"},
    };
    if (!token.empty())
    {
        h.emplace_back("Authorization", token);
    }
    return h;
}

const char *str(yyjson_val *obj, const char *key)
{
    const char *s = yyjson_get_str(yyjson_obj_get(obj, key));
    return s ? s : "";
}

std::string display_name(yyjson_val *user)
{
    const char *global = yyjson_get_str(yyjson_obj_get(user, "global_name"));
    return global && *global ? global : str(user, "username");
}

std::time_t parse_iso(const char *iso)
{
    std::tm tm{};
    if (std::sscanf(iso, "%d-%d-%dT%d:%d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour,
                    &tm.tm_min, &tm.tm_sec) != 6)
    {
        return 0;
    }
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    return timegm(&tm);
}

std::string local_time(std::time_t t)
{
    std::tm local{};
    localtime_r(&t, &local);
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%02d:%02d", local.tm_hour, local.tm_min);
    return buf;
}

// A string field that Discord sometimes sends as a number.
std::string sval(yyjson_val *obj, const char *key)
{
    yyjson_val *v = yyjson_obj_get(obj, key);
    if (yyjson_is_str(v))
    {
        return yyjson_get_str(v);
    }
    if (yyjson_is_uint(v))
    {
        return std::to_string(yyjson_get_uint(v));
    }
    if (yyjson_is_sint(v))
    {
        return std::to_string(yyjson_get_sint(v));
    }
    return {};
}

// Snowflakes grow with time; compare them without overflowing.
bool newer(const std::string &a, const std::string &b)
{
    return a.size() != b.size() ? a.size() > b.size() : a > b;
}

uint64_t u64(const std::string &s)
{
    return std::strtoull(s.c_str(), nullptr, 10);
}

std::string avatar_url(yyjson_val *user)
{
    const std::string id = str(user, "id");
    const char *hash = str(user, "avatar");
    if (*hash)
    {
        return "https://cdn.discordapp.com/avatars/" + id + "/" + hash + ".png?size=128";
    }
    const std::string discriminator = str(user, "discriminator");
    const unsigned index = discriminator.empty() || discriminator == "0"
                               ? unsigned((u64(id) >> 22) % 6)
                               : unsigned(u64(discriminator) % 5);
    return "https://cdn.discordapp.com/embed/avatars/" + std::to_string(index) + ".png";
}

// Pictures shown in a message are at most this big (desktop Discord's 550 x 350, x1.2).
constexpr int kMediaW = 660;
constexpr int kMediaH = 420;

void fit(int w, int h, int max_w, int max_h, int &out_w, int &out_h)
{
    if (w <= 0 || h <= 0)
    {
        w = max_w;
        h = max_h * 3 / 4;
    }
    const float scale = std::min({1.0f, float(max_w) / float(w), float(max_h) / float(h)});
    out_w = std::max(1, int(float(w) * scale));
    out_h = std::max(1, int(float(h) * scale));
}

std::string sized(const std::string &proxy, int w, int h, const char *extra)
{
    return proxy + (proxy.find('?') == std::string::npos ? "?" : "&") +
           "width=" + std::to_string(w) + "&height=" + std::to_string(h) + extra;
}

MediaRef media_ref(yyjson_val *media, int max_w, int max_h, bool animate)
{
    MediaRef ref;
    std::string proxy = str(media, "proxy_url");
    if (proxy.empty())
    {
        proxy = str(media, "url");
    }
    if (proxy.empty())
    {
        return ref;
    }
    fit(int(yyjson_get_int(yyjson_obj_get(media, "width"))),
        int(yyjson_get_int(yyjson_obj_get(media, "height"))), max_w, max_h, ref.width, ref.height);
    const bool gif = proxy.find(".gif") != std::string::npos;
    ref.url = animate && gif ? proxy : sized(proxy, ref.width, ref.height, "&format=png");
    return ref;
}

// Tenor names its renditions by the last letters of the id: AAAAe is a still, AAAAC the GIF.
std::string tenor_gif(const std::string &url)
{
    const size_t host = url.find("media.tenor.com/");
    if (host == std::string::npos)
    {
        return {};
    }
    const size_t start = host + 16;
    const size_t slash = url.find('/', start);
    const size_t dot = url.rfind('.');
    if (slash == std::string::npos || slash - start < 6 || dot == std::string::npos || dot < slash)
    {
        return {};
    }
    return url.substr(0, slash - 5) + "AAAAC" + url.substr(slash, dot - slash) + ".gif";
}

Attachment parse_attachment(yyjson_val *a)
{
    Attachment att;
    att.filename = str(a, "filename");
    att.url = str(a, "url");
    att.size = yyjson_get_uint(yyjson_obj_get(a, "size"));
    const std::string type = str(a, "content_type");
    const std::string proxy = str(a, "proxy_url");
    const int w = int(yyjson_get_int(yyjson_obj_get(a, "width")));
    const int h = int(yyjson_get_int(yyjson_obj_get(a, "height")));
    if (type.rfind("image/", 0) == 0 || (w > 0 && type.rfind("video/", 0) != 0))
    {
        fit(w, h, kMediaW, kMediaH, att.media.width, att.media.height);
        if (type == "image/gif")
        {
            att.media.url = proxy;
        }
        else
        {
            const bool native = type == "image/png" || type == "image/jpeg";
            att.media.url =
                sized(proxy, att.media.width, att.media.height, native ? "" : "&format=png");
        }
    }
    else if (type.rfind("video/", 0) == 0)
    {
        fit(w, h, kMediaW, kMediaH, att.media.width, att.media.height);
        att.media.url = sized(proxy, att.media.width, att.media.height, "&format=jpeg");
        att.media.video = true;
    }
    return att;
}

Embed parse_embed(yyjson_val *e)
{
    Embed embed;
    embed.type = str(e, "type");
    embed.url = str(e, "url");
    embed.title = str(e, "title");
    embed.description = str(e, "description");
    embed.provider = str(yyjson_obj_get(e, "provider"), "name");
    embed.author = str(yyjson_obj_get(e, "author"), "name");
    embed.author_icon = str(yyjson_obj_get(e, "author"), "proxy_icon_url");
    if (!embed.author_icon.empty())
    {
        embed.author_icon = sized(embed.author_icon, 36, 36, "&format=png");
    }
    embed.footer = str(yyjson_obj_get(e, "footer"), "text");
    yyjson_val *color = yyjson_obj_get(e, "color");
    embed.has_color = yyjson_is_int(color);
    embed.color = uint32_t(yyjson_get_int(color));
    size_t i;
    size_t n;
    yyjson_val *f;
    yyjson_arr_foreach(yyjson_obj_get(e, "fields"), i, n, f)
    {
        embed.fields.push_back(
            {str(f, "name"), str(f, "value"), yyjson_get_bool(yyjson_obj_get(f, "inline"))});
    }
    yyjson_val *image = yyjson_obj_get(e, "image");
    yyjson_val *thumb = yyjson_obj_get(e, "thumbnail");
    yyjson_val *video = yyjson_obj_get(e, "video");
    if (embed.type == "gifv")
    {
        yyjson_val *dims = video ? video : thumb;
        fit(int(yyjson_get_int(yyjson_obj_get(dims, "width"))),
            int(yyjson_get_int(yyjson_obj_get(dims, "height"))), kMediaW, kMediaH,
            embed.image.width, embed.image.height);
        embed.image.url = tenor_gif(str(thumb, "url"));
        if (embed.image.url.empty())
        {
            embed.image = media_ref(thumb, kMediaW, kMediaH, true);
        }
    }
    else if (embed.type == "image")
    {
        embed.image = media_ref(thumb ? thumb : image, kMediaW, kMediaH, true);
    }
    else if (embed.type == "video" && !image)
    {
        embed.image = media_ref(thumb, 600, 338, false);
        embed.image.video = true;
    }
    else
    {
        embed.image = media_ref(image, 600, 320, true);
        embed.thumbnail = media_ref(thumb, 120, 120, true);
    }
    return embed;
}

Message parse_message(yyjson_val *m)
{
    Message msg;
    msg.id = str(m, "id");
    msg.type = int(yyjson_get_int(yyjson_obj_get(m, "type")));
    yyjson_val *author = yyjson_obj_get(m, "author");
    msg.author_id = str(author, "id");
    msg.author = display_name(author);
    msg.avatar_url = avatar_url(author);
    msg.bot = yyjson_get_bool(yyjson_obj_get(author, "bot"));
    msg.content = str(m, "content");
    msg.timestamp = parse_iso(str(m, "timestamp"));
    msg.time = local_time(std::time_t(msg.timestamp));
    msg.edited = yyjson_is_str(yyjson_obj_get(m, "edited_timestamp"));
    size_t idx;
    size_t max;
    yyjson_val *v;
    yyjson_arr_foreach(yyjson_obj_get(m, "mentions"), idx, max, v)
    {
        msg.mentions.push_back({str(v, "id"), display_name(v)});
    }
    yyjson_arr_foreach(yyjson_obj_get(m, "attachments"), idx, max, v)
    {
        msg.attachments.push_back(parse_attachment(v));
    }
    yyjson_arr_foreach(yyjson_obj_get(m, "embeds"), idx, max, v)
    {
        msg.embeds.push_back(parse_embed(v));
    }
    yyjson_arr_foreach(yyjson_obj_get(m, "reactions"), idx, max, v)
    {
        yyjson_val *emoji = yyjson_obj_get(v, "emoji");
        Reaction r;
        r.name = str(emoji, "name");
        r.emoji_url = md::reaction_emoji_url(r.name, str(emoji, "id"),
                                             yyjson_get_bool(yyjson_obj_get(emoji, "animated")));
        r.count = int(yyjson_get_int(yyjson_obj_get(v, "count")));
        r.me = yyjson_get_bool(yyjson_obj_get(v, "me"));
        msg.reactions.push_back(r);
    }
    yyjson_arr_foreach(yyjson_obj_get(m, "sticker_items"), idx, max, v)
    {
        Sticker sticker;
        sticker.name = str(v, "name");
        const int format = int(yyjson_get_int(yyjson_obj_get(v, "format_type")));
        if (format != 3)
        {
            sticker.url = std::string("https://media.discordapp.net/stickers/") + str(v, "id") +
                          (format == 4 ? ".gif" : ".png") + "?size=240";
        }
        msg.stickers.push_back(sticker);
    }
    yyjson_val *ref = yyjson_obj_get(m, "referenced_message");
    if (yyjson_is_obj(ref))
    {
        yyjson_val *ra = yyjson_obj_get(ref, "author");
        msg.reply = true;
        msg.reply_author_id = str(ra, "id");
        msg.reply_author = display_name(ra);
        msg.reply_avatar = avatar_url(ra);
        msg.reply_content = str(ref, "content");
        if (msg.reply_content.empty())
        {
            msg.reply_content = yyjson_arr_size(yyjson_obj_get(ref, "attachments")) > 0
                                    ? i18n::T("Eki görüntülemek için tıkla")
                                    : "Mesaj";
        }
    }
    else if (yyjson_is_obj(yyjson_obj_get(m, "message_reference")) && msg.type == 19)
    {
        msg.reply = true;
        msg.reply_content = "Orijinal mesaj silindi";
    }
    return msg;
}

uint32_t role_color(const Guild *guild, const std::vector<std::string> &roles)
{
    if (!guild)
    {
        return 0;
    }
    uint32_t color = 0;
    int best = -1;
    for (const Role &r : guild->roles)
    {
        if (r.color != 0 && r.position > best &&
            std::find(roles.begin(), roles.end(), r.id) != roles.end())
        {
            best = r.position;
            color = r.color;
        }
    }
    return color;
}

std::vector<std::string> string_list(yyjson_val *arr)
{
    std::vector<std::string> out;
    size_t i;
    size_t n;
    yyjson_val *v;
    yyjson_arr_foreach(arr, i, n, v)
    {
        if (yyjson_get_str(v))
        {
            out.push_back(yyjson_get_str(v));
        }
    }
    return out;
}

void refresh_guild(Guild &g)
{
    g.unread = false;
    g.mentions = 0;
    for (const Channel &c : g.channels)
    {
        g.unread = g.unread || (c.unread && !c.muted);
        g.mentions += c.mentions;
    }
    if (g.muted)
    {
        g.unread = false;
    }
}

Guild *find_guild(Snapshot &s, const std::string &id)
{
    for (Guild &g : s.guilds)
    {
        if (g.id == id)
        {
            return &g;
        }
    }
    return nullptr;
}

Channel *find_channel(Snapshot &s, const std::string &id, Guild **owner = nullptr)
{
    for (Guild &g : s.guilds)
    {
        for (Channel &c : g.channels)
        {
            if (c.id == id)
            {
                if (owner)
                {
                    *owner = &g;
                }
                return &c;
            }
        }
    }
    return nullptr;
}

// RSA-OAEP(SHA-256) key pair for Discord's remote-auth (QR) handshake.
class RemoteAuthKey
{
  public:
    RemoteAuthKey()
    {
        EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
        if (ctx && EVP_PKEY_keygen_init(ctx) > 0 && EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, 2048) > 0)
        {
            EVP_PKEY_keygen(ctx, &key_);
        }
        EVP_PKEY_CTX_free(ctx);
    }
    ~RemoteAuthKey()
    {
        EVP_PKEY_free(key_);
    }
    bool ok() const
    {
        return key_ != nullptr;
    }

    std::string public_spki_base64() const
    {
        int len = i2d_PUBKEY(key_, nullptr);
        std::vector<unsigned char> der(size_t(len > 0 ? len : 0));
        unsigned char *p = der.data();
        i2d_PUBKEY(key_, &p);
        return util::base64(der.data(), der.size());
    }

    bool decrypt(const std::string &b64, std::vector<unsigned char> &out) const
    {
        std::vector<unsigned char> in;
        if (!util::unbase64(b64, in))
        {
            return false;
        }
        EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new(key_, nullptr);
        size_t len = 0;
        bool ok = ctx && EVP_PKEY_decrypt_init(ctx) > 0 &&
                  EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_OAEP_PADDING) > 0 &&
                  EVP_PKEY_CTX_set_rsa_oaep_md(ctx, EVP_sha256()) > 0 &&
                  EVP_PKEY_CTX_set_rsa_mgf1_md(ctx, EVP_sha256()) > 0 &&
                  EVP_PKEY_decrypt(ctx, nullptr, &len, in.data(), in.size()) > 0;
        if (ok)
        {
            out.resize(len);
            ok = EVP_PKEY_decrypt(ctx, out.data(), &len, in.data(), in.size()) > 0;
            out.resize(len);
        }
        EVP_PKEY_CTX_free(ctx);
        return ok;
    }

  private:
    EVP_PKEY *key_ = nullptr;
};

bool read_text(const std::string &path, std::string &out)
{
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f)
    {
        return false;
    }
    char buf[512];
    size_t n = std::fread(buf, 1, sizeof(buf), f);
    std::fclose(f);
    out.assign(buf, n);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' '))
    {
        out.pop_back();
    }
    return !out.empty();
}

void write_text(const std::string &path, const std::string &text)
{
    FILE *f = std::fopen(path.c_str(), "wb");
    if (f)
    {
        std::fwrite(text.data(), 1, text.size(), f);
        std::fclose(f);
    }
}

} // namespace

Client &Client::instance()
{
    static Client client;
    return client;
}

void Client::start(const std::string &ca_file, const std::string &session_file,
                   const std::string &import_file)
{
    if (worker_.joinable())
    {
        return;
    }
    net::set_ca_file(ca_file);
    session_file_ = session_file;
    import_file_ = import_file;
    worker_ = std::thread([this] { run(); });
}

void Client::start_web(uint16_t port)
{
    bool ok =
        web_.start(port, [this](const std::string &method, const std::string &path,
                                const std::string &body) { return web_api(method, path, body); });
    std::string ip = ok ? net::local_ipv4() : std::string();
    std::string url =
        "http://" + (ip.empty() ? std::string("<PS5 IP adresi>") : ip) + ":" + std::to_string(port);
    std::string error = web_.error();
    std::printf("[dc] web sign-in %s\n", ok ? url.c_str() : error.c_str());
    update(
        [&](Snapshot &s)
        {
            s.web_url = ok ? url : std::string();
            s.web_error = ok ? std::string() : error;
        });
}

net::HttpReply Client::web_api(const std::string &method, const std::string &path,
                               const std::string &body)
{
    net::HttpReply reply;
    if (method == "GET" && (path == "/" || path.rfind("/?", 0) == 0))
    {
        reply.content_type = "text/html; charset=utf-8";
        reply.body = web::kSignInPage;
        for (const char *text : web::kSignInTexts)
        {
            const std::string translated = i18n::T(text);
            for (size_t at = reply.body.find(text); at != std::string::npos;
                 at = reply.body.find(text, at + translated.size()))
            {
                reply.body.replace(at, std::strlen(text), translated);
            }
        }
        return reply;
    }
    if (method == "GET" && path == "/api/state")
    {
        std::lock_guard<std::mutex> hold(lock_);
        const char *state = state_.state == State::ready   ? "ready"
                            : state_.state == State::login ? "login"
                                                           : "busy";
        reply.body =
            std::string("{\"state\":\"") + state + "\",\"me\":\"" + json_escape(state_.me) + "\"}";
        return reply;
    }
    if (method == "POST" && (path == "/api/login" || path == "/api/mfa"))
    {
        reply.body = web_login(path, body);
        return reply;
    }
    if (method == "POST" && path == "/api/token")
    {
        yyjson_doc *doc = yyjson_read(body.data(), body.size(), 0);
        std::string token = str(yyjson_doc_get_root(doc), "token");
        yyjson_doc_free(doc);
        if (token.size() > 1 && token.front() == '"' && token.back() == '"')
        {
            token = token.substr(1, token.size() - 2);
        }
        if (token.empty())
        {
            reply.status = 400;
            reply.body = "{\"message\":\"Token boş\"}";
            return reply;
        }
        {
            std::lock_guard<std::mutex> hold(lock_);
            pending_token_ = token;
        }
        reply.body = "{\"ok\":true}";
        return reply;
    }
    reply.status = 404;
    reply.body = "{}";
    return reply;
}

// Runs a sign-in step for the page: Discord's reply goes back as is, except that
// a token never leaves the console; the page only learns that it worked.
std::string Client::web_login(const std::string &path, const std::string &body)
{
    yyjson_doc *in = yyjson_read(body.data(), body.size(), 0);
    yyjson_val *root = yyjson_doc_get_root(in);
    net::Headers headers = api_headers({});
    if (fingerprint_.empty())
    {
        net::Response r =
            net::request("GET", std::string(kApi) + "/experiments", api_headers({}), {});
        yyjson_doc *doc = yyjson_read(r.body.data(), r.body.size(), 0);
        fingerprint_ = str(yyjson_doc_get_root(doc), "fingerprint");
        yyjson_doc_free(doc);
    }
    if (!fingerprint_.empty())
    {
        headers.emplace_back("X-Fingerprint", fingerprint_);
    }
    if (*str(root, "captcha_key"))
    {
        headers.emplace_back("X-Captcha-Key", str(root, "captcha_key"));
        headers.emplace_back("X-Captcha-Rqtoken", str(root, "captcha_rqtoken"));
        headers.emplace_back("X-Captcha-Session-Id", str(root, "captcha_session_id"));
    }
    std::string url;
    std::string request;
    if (path == "/api/login")
    {
        url = std::string(kApi) + "/auth/login";
        request = "{\"login\":\"" + json_escape(str(root, "login")) + "\",\"password\":\"" +
                  json_escape(str(root, "password")) +
                  "\",\"undelete\":false,\"login_source\":null,\"gift_code_sku_id\":null}";
    }
    else
    {
        url = std::string(kApi) + "/auth/mfa/totp";
        request = "{\"code\":\"" + json_escape(str(root, "code")) + "\",\"ticket\":\"" +
                  json_escape(str(root, "ticket")) +
                  "\",\"login_source\":null,\"gift_code_sku_id\":null}";
    }
    yyjson_doc_free(in);

    net::Response r = net::request("POST", url, headers, request);
    std::printf("[dc] web %s -> HTTP %d\n", path.c_str(), r.status);
    if (r.status == 0)
    {
        return "{\"message\":\"PS5 Discord'a bağlanamadı: " + json_escape(r.error) + "\"}";
    }
    yyjson_doc *out = yyjson_read(r.body.data(), r.body.size(), 0);
    std::string token = str(yyjson_doc_get_root(out), "token");
    yyjson_doc_free(out);
    if (!token.empty())
    {
        std::lock_guard<std::mutex> hold(lock_);
        pending_token_ = token;
        return "{\"ok\":true}";
    }
    return r.body.empty() ? std::string("{\"message\":\"HTTP ") + std::to_string(r.status) + "\"}"
                          : r.body;
}

bool Client::take_pending_token()
{
    std::lock_guard<std::mutex> hold(lock_);
    if (pending_token_.empty())
    {
        return false;
    }
    token_ = std::move(pending_token_);
    pending_token_.clear();
    return true;
}

bool Client::snapshot_if_newer(uint64_t revision, Snapshot &out)
{
    std::lock_guard<std::mutex> hold(lock_);
    if (state_.revision == revision)
    {
        return false;
    }
    out = state_;
    return true;
}

void Client::post(std::function<void()> command)
{
    std::lock_guard<std::mutex> hold(lock_);
    commands_.push_back(std::move(command));
}

void Client::open_channel(const std::string &channel_id)
{
    post(
        [this, channel_id]
        {
            bool forum = false;
            {
                std::lock_guard<std::mutex> hold(lock_);
                if (const Channel *c = find_channel(state_, channel_id))
                {
                    forum = c->type == 15 || c->type == 16;
                }
            }
            if (forum)
            {
                load_posts(channel_id);
            }
            else
            {
                load_messages(channel_id);
            }
        });
}

void Client::open_post(const std::string &forum_id, const std::string &post_id)
{
    post([this, forum_id, post_id] { load_messages(post_id, forum_id); });
}

void Client::send_message(const std::string &text)
{
    post(
        [this, text]
        {
            std::string channel;
            {
                std::lock_guard<std::mutex> hold(lock_);
                channel = state_.channel_id;
            }
            if (channel.empty() || text.empty())
            {
                return;
            }
            std::string body =
                "{\"content\":\"" + json_escape(text) + "\",\"tts\":false,\"flags\":0}";
            net::Response r =
                net::request("POST", std::string(kApi) + "/channels/" + channel + "/messages",
                             api_headers(token_), body);
            if (r.status != 200)
            {
                update(
                    [&](Snapshot &s) {
                        s.channel_error =
                            i18n::T("Mesaj gönderilemedi (%s)", std::to_string(r.status));
                    });
            }
        });
}

void Client::set_invite_source(const std::string &config_url, const std::string &counter_file)
{
    invite_config_ = config_url;
    invite_counter_ = counter_file;
    voice_file_ = counter_file;
    const auto slash = voice_file_.rfind('/');
    voice_file_ =
        (slash == std::string::npos ? std::string() : voice_file_.substr(0, slash)) + "/voice.txt";
}

void Client::notify(const std::string &text)
{
    if (text.empty())
    {
        return;
    }
    using clock = std::chrono::steady_clock;
    static int burst = 0;
    static clock::time_point window{};
    const auto now = clock::now();
    if (now - window > std::chrono::seconds(8))
    {
        burst = 0;
        window = now;
    }
    if (burst >= 3)
    {
        return;
    }
    ++burst;
    NotifyRequest req;
    std::memset(&req, 0, sizeof req);
    std::snprintf(req.message, sizeof req.message, "PSCord: %s", text.c_str());
    sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
}

void Client::remember_voice()
{
    if (voice_file_.empty())
    {
        return;
    }
    if (voice_channel_.empty())
    {
        std::remove(voice_file_.c_str());
        return;
    }
    write_text(voice_file_, voice_guild_ + "\n" + voice_channel_ + "\n");
}

void Client::load_remembered_voice()
{
    if (!voice_channel_.empty() || voice_file_.empty())
    {
        return;
    }
    std::string text;
    if (!read_text(voice_file_, text))
    {
        return;
    }
    const auto nl = text.find('\n');
    if (nl == std::string::npos)
    {
        return;
    }
    voice_guild_ = text.substr(0, nl);
    voice_channel_ = text.substr(nl + 1);
    const auto end = voice_channel_.find('\n');
    if (end != std::string::npos)
    {
        voice_channel_.resize(end);
    }
}

void Client::rejoin_voice()
{
    if (voice_channel_.empty())
    {
        return;
    }
    voice_session_.clear();
    voice_token_.clear();
    voice_endpoint_.clear();
    voice_armed_session_.clear();
    std::printf("[voice] rejoin guild=%s channel=%s\n", voice_guild_.c_str(),
                voice_channel_.c_str());
    gateway_send("{\"op\":4,\"d\":{\"guild_id\":\"" + voice_guild_ + "\",\"channel_id\":\"" +
                 voice_channel_ + "\",\"self_mute\":" + (voice_muted_ ? "true" : "false") +
                 ",\"self_deaf\":false}}");
    const std::string guild = voice_guild_;
    const std::string channel = voice_channel_;
    update(
        [&](Snapshot &s)
        {
            s.voice_guild_id = guild;
            s.voice_channel_id = channel;
            s.voice_muted = voice_muted_;
        });
    notify(i18n::T("Sesli kanala yeniden bağlanılıyor"));
}

void Client::check_invite()
{
    if (invite_checked_ || invite_config_.empty())
    {
        return;
    }
    invite_checked_ = true;
    int launches = 0;
    if (std::FILE *f = std::fopen(invite_counter_.c_str(), "r"))
    {
        if (std::fscanf(f, "%d", &launches) != 1)
        {
            launches = 0;
        }
        std::fclose(f);
    }
    ++launches;
    if (std::FILE *f = std::fopen(invite_counter_.c_str(), "w"))
    {
        std::fprintf(f, "%d\n", launches);
        std::fclose(f);
    }
    if (launches % kInviteEvery != 1)
    {
        return;
    }
    offer_invite(true);
}

void Client::show_community_invite()
{
    post([this] { offer_invite(false); });
}

void Client::offer_invite(bool skip_if_member)
{
    std::string code = kDefaultInvite;
    if (!invite_config_.empty())
    {
        net::Response config = net::request("GET", invite_config_, {{"User-Agent", "PSCord"}});
        yyjson_doc *doc =
            config.status == 200 ? yyjson_read(config.body.data(), config.body.size(), 0) : nullptr;
        std::string fetched = doc ? str(yyjson_doc_get_root(doc), "invite") : "";
        yyjson_doc_free(doc);
        for (const char *prefix : {"https://", "http://", "discord.gg/", "discord.com/invite/"})
        {
            if (fetched.compare(0, std::strlen(prefix), prefix) == 0)
            {
                fetched.erase(0, std::strlen(prefix));
            }
        }
        if (!fetched.empty() && fetched.find_first_of("/?# ") == std::string::npos)
        {
            code = fetched;
        }
    }

    net::Response r = net::request(
        "GET", std::string(kApi) + "/invites/" + code + "?with_counts=true", api_headers({}), {});
    yyjson_doc *doc = r.status == 200 ? yyjson_read(r.body.data(), r.body.size(), 0) : nullptr;
    if (!doc)
    {
        std::printf("[dc] invite %s not found (status %d)\n", code.c_str(), r.status);
        return;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *guild = yyjson_obj_get(root, "guild");
    Invite invite;
    invite.code = code;
    invite.guild_name = str(guild, "name");
    const std::string guild_id = str(guild, "id");
    const std::string icon = str(guild, "icon");
    if (!icon.empty())
    {
        invite.icon_url =
            "https://cdn.discordapp.com/icons/" + guild_id + "/" + icon + ".png?size=128";
    }
    invite.members = int(yyjson_get_int(yyjson_obj_get(root, "approximate_member_count")));
    invite.online = int(yyjson_get_int(yyjson_obj_get(root, "approximate_presence_count")));
    yyjson_doc_free(doc);
    update(
        [&](Snapshot &s)
        {
            if (skip_if_member)
            {
                for (const Guild &g : s.guilds)
                {
                    if (g.id == guild_id)
                    {
                        return;
                    }
                }
            }
            s.invite = invite;
        });
}

void Client::join_invite()
{
    post(
        [this]
        {
            std::string code;
            update(
                [&](Snapshot &s)
                {
                    code = s.invite.code;
                    s.invite.join = Invite::Join::joining;
                });
            net::Headers headers = api_headers(token_);
            // Says the invite was opened from a link, as the desktop client does.
            headers.emplace_back("X-Context-Properties",
                                 "eyJsb2NhdGlvbiI6Ik1hcmtkb3duIExpbmsifQ==");
            net::Response r =
                net::request("POST", std::string(kApi) + "/invites/" + code, headers, "{}");
            std::printf("[dc] join invite %s: %d\n", code.c_str(), r.status);
            update(
                [&](Snapshot &s)
                { s.invite.join = r.status == 200 ? Invite::Join::joined : Invite::Join::failed; });
        });
}

void Client::dismiss_invite()
{
    update([](Snapshot &s) { s.invite = Invite{}; });
}

void Client::join_voice(const std::string &guild_id, const std::string &channel_id)
{
    post(
        [this, guild_id, channel_id]
        {
            if (guild_id.empty() || (guild_id == voice_guild_ && channel_id == voice_channel_))
            {
                return;
            }
            voice::bg::leave(nullptr);
            voice::Engine::instance().stop();
            // Same guild keeps the voice server token. Discord often sends only a
            // new session id when the channel changes, and the old token still works.
            if (guild_id != voice_guild_)
            {
                voice_token_.clear();
                voice_endpoint_.clear();
            }
            voice_guild_ = guild_id;
            voice_channel_ = channel_id;
            remember_voice();
            voice_session_.clear();
            voice_armed_session_.clear();
            voice_wait_since_ = std::chrono::steady_clock::now();
            voice_wait_retried_ = false;
            gateway_send("{\"op\":4,\"d\":{\"guild_id\":\"" + guild_id + "\",\"channel_id\":\"" +
                         channel_id + "\",\"self_mute\":" + (voice_muted_ ? "true" : "false") +
                         ",\"self_deaf\":false}}");
            update(
                [&](Snapshot &s)
                {
                    s.voice_guild_id = guild_id;
                    s.voice_channel_id = channel_id;
                });
        });
}

void Client::leave_voice()
{
    post(
        [this]
        {
            voice::bg::leave(nullptr);
            voice::Engine::instance().stop();
            if (!voice_guild_.empty())
            {
                gateway_send("{\"op\":4,\"d\":{\"guild_id\":\"" + voice_guild_ +
                             "\",\"channel_id\":null,\"self_mute\":false,\"self_deaf\":false}}");
            }
            voice_guild_.clear();
            voice_channel_.clear();
            voice_session_.clear();
            voice_token_.clear();
            voice_endpoint_.clear();
            voice_armed_session_.clear();
            remember_voice();
            update(
                [](Snapshot &s)
                {
                    s.voice_guild_id.clear();
                    s.voice_channel_id.clear();
                });
        });
}

void Client::set_voice_muted(bool muted)
{
    post(
        [this, muted]
        {
            voice_muted_ = muted;
            voice::Engine::instance().set_muted(muted);
            if (voice::bg::available())
            {
                voice::bg::set_muted(muted, nullptr);
            }
            if (!voice_channel_.empty())
            {
                gateway_send("{\"op\":4,\"d\":{\"guild_id\":\"" + voice_guild_ +
                             "\",\"channel_id\":\"" + voice_channel_ + "\",\"self_mute\":" +
                             (muted ? "true" : "false") + ",\"self_deaf\":false}}");
            }
            update([&](Snapshot &s) { s.voice_muted = muted; });
        });
}

void Client::maybe_start_voice()
{
    if (voice_channel_.empty() || voice_session_.empty() || voice_token_.empty() ||
        voice_endpoint_.empty())
    {
        std::printf("[voice] waiting channel=%s session=%s token=%s endpoint=%s\n",
                    voice_channel_.empty() ? "no" : "yes", voice_session_.empty() ? "no" : "yes",
                    voice_token_.empty() ? "no" : "yes", voice_endpoint_.empty() ? "no" : "yes");
        return;
    }
    if (voice_session_ == voice_armed_session_)
    {
        return; // mute and deafen updates repeat the same session
    }
    voice::JoinInfo info;
    info.endpoint = voice_endpoint_;
    info.token = voice_token_;
    info.session_id = voice_session_;
    info.server_id = voice_guild_;
    info.channel_id = voice_channel_;
    info.user_id = me_id_;
    voice_armed_session_ = voice_session_;
    // The daemon is not a title, so the call keeps going after another game launches.
    // If it cannot open the speakers, stay on the in-app engine instead.
    std::string status;
    std::string error;
    const bool up = voice::bg::ensure_running(voice::bg::kElfAppPath, &status);
    std::printf("[voice] background: %s\n", status.c_str());
    if (up && voice::bg::join(info, voice_muted_, &error))
    {
        const std::string line = voice::bg::status_line();
        std::printf("[voice] background status: %s\n", line.c_str());
        if (line.find("audio=1") != std::string::npos)
        {
            voice::Engine::instance().stop();
            return;
        }
        voice::bg::leave(nullptr);
        std::printf("[voice] background has no speakers; using the in-app engine\n");
    }
    else if (up)
    {
        std::printf("[voice] background join failed: %s\n", error.c_str());
    }
    voice::Engine::instance().start(info);
}

void Client::voice_state(const void *state, const std::string &guild_id)
{
    auto *d = static_cast<yyjson_val *>(const_cast<void *>(state));
    const std::string user = str(d, "user_id");
    const std::string channel = str(d, "channel_id");
    yyjson_val *member = yyjson_obj_get(d, "member");
    yyjson_val *member_user = yyjson_obj_get(member, "user");
    VoiceUser joined;
    joined.id = user;
    const bool muted = yyjson_get_bool(yyjson_obj_get(d, "self_mute")) ||
                       yyjson_get_bool(yyjson_obj_get(d, "mute"));
    const bool deafened = yyjson_get_bool(yyjson_obj_get(d, "self_deaf")) ||
                          yyjson_get_bool(yyjson_obj_get(d, "deaf"));
    if (member_user)
    {
        const char *nick = yyjson_get_str(yyjson_obj_get(member, "nick"));
        joined.name = nick && *nick ? nick : display_name(member_user);
        joined.avatar_url = avatar_url(member_user);
    }
    joined.muted = muted;
    joined.deafened = deafened;
    update(
        [&](Snapshot &s)
        {
            Guild *g = find_guild(s, guild_id);
            if (!g)
            {
                return;
            }
            for (Channel &c : g->channels)
            {
                auto it = std::find_if(c.voice_users.begin(), c.voice_users.end(),
                                       [&](const VoiceUser &u) { return u.id == user; });
                if (it != c.voice_users.end())
                {
                    if (joined.name.empty())
                    {
                        joined.name = it->name;
                        joined.avatar_url = it->avatar_url;
                    }
                    c.voice_users.erase(it);
                }
            }
            for (Channel &c : g->channels)
            {
                if (c.id == channel)
                {
                    c.voice_users.push_back(joined);
                }
            }
        });
    if (joined.name.empty() && !channel.empty())
    {
        request_members(guild_id, {user});
    }
    if (user != me_id_ || guild_id != voice_guild_)
    {
        return;
    }
    if (channel.empty())
    {
        // Leaving the previous channel arrives as an empty state while the new
        // join is still waiting for its session. Dropping it here used to cancel
        // the move, and the panel stayed on "Connecting".
        if (voice_session_.empty() && !voice_channel_.empty())
        {
            std::printf("[voice] ignore empty state during join guild=%s channel=%s\n",
                        voice_guild_.c_str(), voice_channel_.c_str());
            return;
        }
        std::printf("[voice] left guild=%s\n", guild_id.c_str());
        voice::bg::leave(nullptr);
        voice::Engine::instance().stop();
        voice_guild_.clear();
        voice_channel_.clear();
        voice_session_.clear();
        voice_token_.clear();
        voice_endpoint_.clear();
        voice_armed_session_.clear();
        remember_voice();
        update(
            [](Snapshot &s)
            {
                s.voice_guild_id.clear();
                s.voice_channel_id.clear();
            });
    }
    else if (channel == voice_channel_)
    {
        voice_session_ = str(d, "session_id");
        std::printf("[voice] session %s channel=%s\n", voice_session_.c_str(), channel.c_str());
        maybe_start_voice();
    }
    else
    {
        // Discord (or a bot such as join-to-create) moved us. Follow the move
        // instead of waiting forever for the channel we asked for.
        std::printf("[voice] moved to channel=%s (asked %s)\n", channel.c_str(),
                    voice_channel_.c_str());
        voice::bg::leave(nullptr);
        voice::Engine::instance().stop();
        voice_channel_ = channel;
        voice_session_ = str(d, "session_id");
        voice_armed_session_.clear();
        voice_wait_since_ = std::chrono::steady_clock::now();
        voice_wait_retried_ = false;
        remember_voice();
        update([&](Snapshot &s) { s.voice_channel_id = channel; });
        maybe_start_voice();
    }
}

// Discord sometimes never answers op 4 with VOICE_SERVER_UPDATE. Ask once
// more, then give up so the panel does not sit on "Connecting" forever.
void Client::voice_watchdog()
{
    if (voice_channel_.empty() || !voice_armed_session_.empty())
    {
        voice_wait_since_ = {};
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (voice_wait_since_ == std::chrono::steady_clock::time_point{})
    {
        voice_wait_since_ = now;
        voice_wait_retried_ = false;
        return;
    }
    const auto waited =
        std::chrono::duration_cast<std::chrono::seconds>(now - voice_wait_since_).count();
    if (!voice_wait_retried_ && waited >= 8)
    {
        voice_wait_retried_ = true;
        std::printf("[voice] no voice server after %llds, asking again session=%s token=%s\n",
                    (long long)waited, voice_session_.empty() ? "no" : "yes",
                    voice_token_.empty() ? "no" : "yes");
        voice_token_.clear();
        voice_endpoint_.clear();
        gateway_send("{\"op\":4,\"d\":{\"guild_id\":\"" + voice_guild_ + "\",\"channel_id\":\"" +
                     voice_channel_ + "\",\"self_mute\":" + (voice_muted_ ? "true" : "false") +
                     ",\"self_deaf\":false}}");
    }
    else if (voice_wait_retried_ && waited >= 20)
    {
        std::printf("[voice] giving up guild=%s channel=%s\n", voice_guild_.c_str(),
                    voice_channel_.c_str());
        gateway_send("{\"op\":4,\"d\":{\"guild_id\":\"" + voice_guild_ +
                     "\",\"channel_id\":null,\"self_mute\":false,\"self_deaf\":false}}");
        voice_guild_.clear();
        voice_channel_.clear();
        voice_session_.clear();
        voice_token_.clear();
        voice_endpoint_.clear();
        voice_wait_since_ = {};
        remember_voice();
        update(
            [](Snapshot &s)
            {
                s.voice_guild_id.clear();
                s.voice_channel_id.clear();
            });
        notify(i18n::T("Discord ses sunucusu yanıt vermedi, kanala tekrar gir"));
    }
}

void Client::logout()
{
    post(
        [this]
        {
            voice::Engine::instance().stop();
            logout_requested_ = true;
            reconnect_now_ = true;
            token_.clear();
            std::remove(session_file_.c_str());
            if (gateway_)
            {
                static_cast<net::WebSocket *>(gateway_)->close();
            }
        });
}

void Client::retry_connect()
{
    post(
        [this]
        {
            reconnect_now_ = true;
            if (gateway_)
            {
                static_cast<net::WebSocket *>(gateway_)->close();
            }
            update(
                [](Snapshot &s)
                {
                    s.state = State::connecting;
                    s.status = i18n::T("Discord'a bağlanılıyor...");
                });
        });
}

void Client::process_commands()
{
    for (;;)
    {
        std::function<void()> command;
        {
            std::lock_guard<std::mutex> hold(lock_);
            if (commands_.empty())
            {
                return;
            }
            command = std::move(commands_.front());
            commands_.pop_front();
        }
        command();
    }
}

void Client::run()
{
    for (;;)
    {
        logout_requested_ = false;
        std::string imported;
        if (token_.empty() && !import_file_.empty() && read_text(import_file_, imported))
        {
            std::remove(import_file_.c_str());
            if (imported.size() > 1 && imported.front() == '"' && imported.back() == '"')
            {
                imported = imported.substr(1, imported.size() - 2);
            }
            token_ = imported;
            write_text(session_file_, token_);
        }
        if (token_.empty() && !read_text(session_file_, token_))
        {
            update(
                [](Snapshot &s)
                {
                    Snapshot fresh;
                    fresh.revision = s.revision;
                    fresh.web_url = s.web_url;
                    fresh.web_error = s.web_error;
                    s = fresh;
                    s.state = State::login;
                    s.status = i18n::T("QR kodu hazırlanıyor...");
                });
            if (!login_with_qr())
            {
                std::this_thread::sleep_for(std::chrono::seconds(3));
                continue;
            }
            write_text(session_file_, token_);
        }
        run_gateway();
        if (!logout_requested_ && !token_.empty())
        {
            // Pause between attempts unless the player asked to retry now.
            for (int i = 0; i < 50 && !reconnect_now_ && !logout_requested_; ++i)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
        reconnect_now_ = false;
    }
}

bool Client::login_with_qr()
{
    RemoteAuthKey key;
    if (!key.ok())
    {
        update([](Snapshot &s) { s.status = i18n::T("Anahtar üretilemedi"); });
        return false;
    }
    net::WebSocket ws;
    if (!ws.connect("wss://remote-auth-gateway.discord.gg/?v=2",
                    {{"Origin", "https://discord.com"}, {"User-Agent", kUserAgent}}))
    {
        std::string error = ws.error();
        update([&](Snapshot &s) { s.status = i18n::T("Bağlanılamadı: ") + error; });
        return false;
    }
    int heartbeat_ms = 41250;
    auto next_beat = Clock::now() + std::chrono::milliseconds(heartbeat_ms);
    for (;;)
    {
        process_commands();
        if (take_pending_token())
        {
            ws.close();
            return true;
        }
        if (!import_file_.empty())
        {
            std::FILE *dropped = std::fopen(import_file_.c_str(), "rb");
            if (dropped)
            {
                std::fclose(dropped);
                ws.close();
                return false;
            }
        }
        int wait =
            int(std::chrono::duration_cast<std::chrono::milliseconds>(next_beat - Clock::now())
                    .count());
        std::string text;
        net::WebSocket::Result result = ws.receive(text, std::clamp(wait, 0, 500));
        if (Clock::now() >= next_beat)
        {
            ws.send_text("{\"op\":\"heartbeat\"}");
            next_beat = Clock::now() + std::chrono::milliseconds(heartbeat_ms);
        }
        if (result == net::WebSocket::Result::closed)
        {
            std::string error = ws.error();
            update(
                [&](Snapshot &s)
                {
                    s.qr_url.clear();
                    s.pending_user.clear();
                    s.status = i18n::T("QR kodunun süresi doldu, yenileniyor...");
                });
            return false;
        }
        if (result != net::WebSocket::Result::message)
        {
            continue;
        }
        yyjson_doc *doc = yyjson_read(text.data(), text.size(), 0);
        yyjson_val *root = yyjson_doc_get_root(doc);
        std::string op = str(root, "op");
        if (op == "hello")
        {
            heartbeat_ms = int(yyjson_get_int(yyjson_obj_get(root, "heartbeat_interval")));
            if (heartbeat_ms <= 0)
            {
                heartbeat_ms = 41250;
            }
            next_beat = Clock::now() + std::chrono::milliseconds(heartbeat_ms);
            ws.send_text("{\"op\":\"init\",\"encoded_public_key\":\"" + key.public_spki_base64() +
                         "\"}");
        }
        else if (op == "nonce_proof")
        {
            std::vector<unsigned char> nonce;
            if (key.decrypt(str(root, "encrypted_nonce"), nonce))
            {
                unsigned char digest[SHA256_DIGEST_LENGTH];
                SHA256(nonce.data(), nonce.size(), digest);
                ws.send_text("{\"op\":\"nonce_proof\",\"proof\":\"" +
                             util::base64(digest, sizeof(digest), true, false) + "\"}");
            }
        }
        else if (op == "pending_remote_init")
        {
            std::string url = std::string("https://discord.com/ra/") + str(root, "fingerprint");
            update(
                [&](Snapshot &s)
                {
                    s.qr_url = url;
                    s.pending_user.clear();
                    s.status = "Telefonda Discord > Profil > QR Kodu Tara";
                });
        }
        else if (op == "pending_ticket")
        {
            std::vector<unsigned char> payload;
            std::string who;
            if (key.decrypt(str(root, "encrypted_user_payload"), payload))
            {
                // id:discriminator:avatar:username
                std::string text_payload(payload.begin(), payload.end());
                size_t last = text_payload.rfind(':');
                who = last == std::string::npos ? text_payload : text_payload.substr(last + 1);
            }
            update(
                [&](Snapshot &s)
                {
                    s.pending_user = who;
                    s.status = i18n::T("Telefonda girişi onayla");
                });
        }
        else if (op == "pending_login")
        {
            std::string ticket = str(root, "ticket");
            yyjson_doc_free(doc);
            update([](Snapshot &s) { s.status = i18n::T("Giriş yapılıyor..."); });
            net::Response r =
                net::request("POST", std::string(kApi) + "/users/@me/remote-auth/login",
                             api_headers({}), "{\"ticket\":\"" + json_escape(ticket) + "\"}");
            yyjson_doc *reply = yyjson_read(r.body.data(), r.body.size(), 0);
            std::string encrypted = str(yyjson_doc_get_root(reply), "encrypted_token");
            yyjson_doc_free(reply);
            std::vector<unsigned char> token;
            if (r.status != 200 || encrypted.empty() || !key.decrypt(encrypted, token))
            {
                std::string why = r.status ? "HTTP " + std::to_string(r.status) : r.error;
                update(
                    [&](Snapshot &s)
                    {
                        s.qr_url.clear();
                        s.status = i18n::T("Giriş başarısız: ") + why;
                    });
                ws.close();
                return false;
            }
            token_.assign(token.begin(), token.end());
            ws.close();
            return true;
        }
        else if (op == "cancel")
        {
            update(
                [](Snapshot &s)
                {
                    s.pending_user.clear();
                    s.status = i18n::T("Giriş telefonda iptal edildi");
                });
        }
        yyjson_doc_free(doc);
    }
}

void Client::run_gateway()
{
    load_remembered_voice();
    update(
        [](Snapshot &s)
        {
            s.state = State::connecting;
            s.status = i18n::T("Discord'a bağlanılıyor...");
            s.qr_url.clear();
        });
    net::WebSocket ws;
    struct Attach
    {
        void *&slot;
        ~Attach()
        {
            slot = nullptr;
        }
    } attach{gateway_};
    gateway_ = &ws;
    if (!ws.connect("wss://gateway.discord.gg/?v=9&encoding=json",
                    {{"Origin", "https://discord.com"}, {"User-Agent", kUserAgent}}))
    {
        std::string error = ws.error();
        update(
            [&](Snapshot &s)
            {
                s.state = State::failed;
                s.status = i18n::T("Bağlanılamadı: ") + error;
            });
        return;
    }
    update(
        [](Snapshot &s)
        {
            s.state = State::connecting;
            s.status = i18n::T("Discord yanıtı bekleniyor...");
        });
    constexpr auto kHelloWait = std::chrono::seconds(20);
    constexpr auto kReadyWait = std::chrono::seconds(90);
    auto hello_deadline = Clock::now() + kHelloWait;
    auto ready_deadline = Clock::time_point::max();
    bool got_hello = false;
    bool got_ready = false;
    int heartbeat_ms = 41250;
    auto next_beat = Clock::time_point::max();
    long long sequence = -1;
    for (;;)
    {
        process_commands();
        voice_watchdog();
        if (logout_requested_)
        {
            ws.close();
            return;
        }
        if (!got_hello && Clock::now() >= hello_deadline)
        {
            update(
                [](Snapshot &s)
                {
                    s.state = State::failed;
                    s.status = i18n::T("Discord yanıt vermedi");
                });
            ws.close();
            return;
        }
        if (got_hello && !got_ready && Clock::now() >= ready_deadline)
        {
            update(
                [](Snapshot &s)
                {
                    s.state = State::failed;
                    s.status = i18n::T("Hesap yüklenemedi");
                });
            ws.close();
            return;
        }
        int wait = next_beat == Clock::time_point::max()
                       ? 500
                       : int(std::chrono::duration_cast<std::chrono::milliseconds>(next_beat -
                                                                                   Clock::now())
                                 .count());
        std::string text;
        net::WebSocket::Result result = ws.receive(text, std::clamp(wait, 0, 200));
        if (Clock::now() >= next_beat)
        {
            ws.send_text("{\"op\":1,\"d\":" +
                         (sequence < 0 ? std::string("null") : std::to_string(sequence)) + "}");
            next_beat = Clock::now() + std::chrono::milliseconds(heartbeat_ms);
        }
        if (result == net::WebSocket::Result::closed)
        {
            if (logout_requested_)
            {
                return;
            }
            if (reconnect_now_)
            {
                update(
                    [](Snapshot &s)
                    {
                        s.state = State::connecting;
                        s.status = i18n::T("Discord'a bağlanılıyor...");
                    });
                return;
            }
            int code = ws.close_code();
            std::string error = ws.error();
            if (code == 4004)
            {
                token_.clear();
                std::remove(session_file_.c_str());
            }
            update(
                [&](Snapshot &s)
                {
                    s.state = State::failed;
                    s.status = code == 4004
                                   ? std::string(i18n::T("Oturum geçersiz, tekrar giriş gerekiyor"))
                                   : i18n::T("Bağlantı koptu: ") + error;
                });
            return;
        }
        if (result != net::WebSocket::Result::message)
        {
            continue;
        }
        yyjson_doc *doc = yyjson_read(text.data(), text.size(), 0);
        yyjson_val *root = yyjson_doc_get_root(doc);
        int op = int(yyjson_get_int(yyjson_obj_get(root, "op")));
        yyjson_val *seq = yyjson_obj_get(root, "s");
        if (yyjson_is_int(seq))
        {
            sequence = yyjson_get_sint(seq);
        }
        yyjson_val *d = yyjson_obj_get(root, "d");
        if (op == 10)
        {
            got_hello = true;
            ready_deadline = Clock::now() + kReadyWait;
            heartbeat_ms = int(yyjson_get_int(yyjson_obj_get(d, "heartbeat_interval")));
            next_beat = Clock::now() + std::chrono::milliseconds(heartbeat_ms / 2);
            update(
                [](Snapshot &s)
                {
                    s.state = State::connecting;
                    s.status = i18n::T("Hesap yükleniyor...");
                });
            ws.send_text(
                "{\"op\":2,\"d\":{\"token\":\"" + json_escape(token_) +
                "\",\"properties\":" + properties_json() +
                ",\"presence\":{\"status\":\"online\",\"since\":0,\"activities\":[],\"afk\":false},"
                "\"compress\":false,\"client_state\":{\"guild_versions\":{}}}}");
        }
        else if (op == 1)
        {
            next_beat = Clock::now();
        }
        else if (op == 9)
        {
            token_.clear();
            std::remove(session_file_.c_str());
            update(
                [](Snapshot &s)
                {
                    s.state = State::failed;
                    s.status = i18n::T("Oturum geçersiz, tekrar giriş gerekiyor");
                });
            yyjson_doc_free(doc);
            ws.close();
            return;
        }
        else if (op == 0)
        {
            const std::string type = str(root, "t");
            handle_dispatch(type, d);
            if (type == "READY")
            {
                got_ready = true;
            }
        }
        yyjson_doc_free(doc);
    }
}

void Client::gateway_send(const std::string &text)
{
    if (gateway_)
    {
        static_cast<net::WebSocket *>(gateway_)->send_text(text);
    }
}

void Client::request_members(const std::string &guild_id, const std::vector<std::string> &user_ids)
{
    std::string ids;
    int count = 0;
    for (const std::string &id : user_ids)
    {
        if (!asked_members_.insert(guild_id + ":" + id).second)
        {
            continue;
        }
        ids += (ids.empty() ? "\"" : ",\"") + id + "\"";
        if (++count == 100)
        {
            break;
        }
    }
    if (count > 0)
    {
        gateway_send("{\"op\":8,\"d\":{\"guild_id\":[\"" + guild_id + "\"],\"user_ids\":[" + ids +
                     "],\"presences\":false}}");
    }
}

void Client::subscribe_member_list(const std::string &guild_id, const std::string &channel_id)
{
    member_items_.clear();
    gateway_send("{\"op\":37,\"d\":{\"subscriptions\":{\"" + guild_id +
                 "\":{\"typing\":true,\"activities\":true,\"threads\":true,\"channels\":{\"" +
                 channel_id + "\":[[0,99]]}}}}}");
}

void Client::recolor(Snapshot &s)
{
    if (s.guild_id.empty())
    {
        return;
    }
    const Guild *guild = find_guild(s, s.guild_id);
    const auto &roles = member_roles_[s.guild_id];
    auto color_of = [&](const std::string &user)
    {
        auto it = roles.find(user);
        return it == roles.end() ? 0u : role_color(guild, it->second);
    };
    for (Message &m : s.messages)
    {
        m.color = color_of(m.author_id);
        m.reply_color = color_of(m.reply_author_id);
    }
    for (Member &m : s.members)
    {
        if (!m.group)
        {
            m.color = color_of(m.id);
        }
    }
}

void Client::apply_member_list(const void *data)
{
    auto *d = static_cast<yyjson_val *>(const_cast<void *>(data));
    const std::string guild = str(d, "guild_id");
    auto to_member = [&](yyjson_val *item)
    {
        Member out;
        if (yyjson_val *group = yyjson_obj_get(item, "group"))
        {
            out.group = true;
            out.id = str(group, "id");
            out.count = int(yyjson_get_int(yyjson_obj_get(group, "count")));
            return out;
        }
        yyjson_val *member = yyjson_obj_get(item, "member");
        yyjson_val *user = yyjson_obj_get(member, "user");
        out.id = str(user, "id");
        const char *nick = yyjson_get_str(yyjson_obj_get(member, "nick"));
        out.label = nick && *nick ? nick : display_name(user);
        out.avatar_url = avatar_url(user);
        out.bot = yyjson_get_bool(yyjson_obj_get(user, "bot"));
        out.status = str(yyjson_obj_get(member, "presence"), "status");
        if (out.status.empty())
        {
            out.status = "offline";
        }
        member_roles_[guild][out.id] = string_list(yyjson_obj_get(member, "roles"));
        return out;
    };
    size_t i;
    size_t n;
    yyjson_val *op;
    yyjson_arr_foreach(yyjson_obj_get(d, "ops"), i, n, op)
    {
        const std::string kind = str(op, "op");
        if (kind == "SYNC")
        {
            yyjson_val *range = yyjson_obj_get(op, "range");
            size_t at = size_t(yyjson_get_int(yyjson_arr_get(range, 0)));
            size_t k;
            size_t m;
            yyjson_val *item;
            yyjson_arr_foreach(yyjson_obj_get(op, "items"), k, m, item)
            {
                if (member_items_.size() <= at + k)
                {
                    member_items_.resize(at + k + 1);
                }
                member_items_[at + k] = to_member(item);
            }
        }
        else if (kind == "INSERT" || kind == "UPDATE")
        {
            const size_t index = size_t(yyjson_get_int(yyjson_obj_get(op, "index")));
            Member member = to_member(yyjson_obj_get(op, "item"));
            if (kind == "INSERT" && index <= member_items_.size())
            {
                member_items_.insert(member_items_.begin() + long(index), member);
            }
            else if (index < member_items_.size())
            {
                member_items_[index] = member;
            }
        }
        else if (kind == "DELETE")
        {
            const size_t index = size_t(yyjson_get_int(yyjson_obj_get(op, "index")));
            if (index < member_items_.size())
            {
                member_items_.erase(member_items_.begin() + long(index));
            }
        }
    }
    update(
        [&](Snapshot &s)
        {
            if (s.guild_id != guild)
            {
                return;
            }
            const Guild *g = find_guild(s, guild);
            s.members.clear();
            for (Member m : member_items_)
            {
                if (m.id.empty() && !m.group)
                {
                    continue;
                }
                if (m.group)
                {
                    if (m.id == "online")
                    {
                        m.label = i18n::T("Çevrim içi");
                    }
                    else if (m.id == "offline")
                    {
                        m.label = i18n::T("Çevrim dışı");
                    }
                    else if (g)
                    {
                        for (const Role &r : g->roles)
                        {
                            if (r.id == m.id)
                            {
                                m.label = r.name;
                            }
                        }
                    }
                }
                s.members.push_back(m);
            }
            recolor(s);
        });
}

void Client::mark_read(const std::string &channel_id, const std::string &message_id)
{
    update(
        [&](Snapshot &s)
        {
            Guild *g = nullptr;
            if (Channel *c = find_channel(s, channel_id, &g))
            {
                c->unread = false;
                c->mentions = 0;
                refresh_guild(*g);
            }
        });
    if (!message_id.empty())
    {
        net::request("POST",
                     std::string(kApi) + "/channels/" + channel_id + "/messages/" + message_id +
                         "/ack",
                     api_headers(token_), "{\"token\":null}");
    }
}

void Client::handle_dispatch(const std::string &type, const void *data)
{
    auto *d = static_cast<yyjson_val *>(const_cast<void *>(data));
    size_t idx;
    size_t max;
    yyjson_val *v;
    if (type == "READY")
    {
        yyjson_val *user = yyjson_obj_get(d, "user");
        const std::string me = display_name(user);
        const std::string me_id = str(user, "id");
        const std::string me_avatar = avatar_url(user);
        struct UserInfo
        {
            std::string name;
            std::string avatar;
        };
        std::map<std::string, UserInfo> users;
        yyjson_arr_foreach(yyjson_obj_get(d, "users"), idx, max, v)
        {
            users[str(v, "id")] = {display_name(v), avatar_url(v)};
        }

        std::map<std::string, std::pair<std::string, int>> reads;
        yyjson_val *read_state = yyjson_obj_get(d, "read_state");
        yyjson_val *entries =
            yyjson_is_obj(read_state) ? yyjson_obj_get(read_state, "entries") : read_state;
        yyjson_arr_foreach(entries, idx, max, v)
        {
            reads[str(v, "id")] = {sval(v, "last_message_id"),
                                   int(yyjson_get_int(yyjson_obj_get(v, "mention_count")))};
        }
        std::set<std::string> muted_guilds;
        std::set<std::string> muted_channels;
        yyjson_val *settings = yyjson_obj_get(d, "user_guild_settings");
        entries = yyjson_is_obj(settings) ? yyjson_obj_get(settings, "entries") : settings;
        yyjson_arr_foreach(entries, idx, max, v)
        {
            if (yyjson_get_bool(yyjson_obj_get(v, "muted")))
            {
                muted_guilds.insert(sval(v, "guild_id"));
            }
            size_t i2;
            size_t m2;
            yyjson_val *o;
            yyjson_arr_foreach(yyjson_obj_get(v, "channel_overrides"), i2, m2, o)
            {
                if (yyjson_get_bool(yyjson_obj_get(o, "muted")))
                {
                    muted_channels.insert(sval(o, "channel_id"));
                }
            }
        }
        auto apply_read = [&](Channel &c)
        {
            auto it = reads.find(c.id);
            if (it != reads.end())
            {
                c.unread = !c.last_message_id.empty() && newer(c.last_message_id, it->second.first);
                c.mentions = it->second.second;
            }
            c.muted = muted_channels.count(c.id) > 0;
        };

        Guild dms;
        dms.name = i18n::T("Özel Mesajlar");
        yyjson_arr_foreach(yyjson_obj_get(d, "private_channels"), idx, max, v)
        {
            Channel c;
            c.id = str(v, "id");
            c.type = int(yyjson_get_int(yyjson_obj_get(v, "type")));
            c.name = str(v, "name");
            c.last_message_id = sval(v, "last_message_id");
            size_t i2;
            size_t m2;
            yyjson_val *r;
            std::string names;
            yyjson_arr_foreach(yyjson_obj_get(v, "recipients"), i2, m2, r)
            {
                names += (names.empty() ? "" : ", ") + display_name(r);
                if (c.icon_url.empty())
                {
                    c.icon_url = avatar_url(r);
                }
            }
            yyjson_arr_foreach(yyjson_obj_get(v, "recipient_ids"), i2, m2, r)
            {
                auto it = users.find(yyjson_get_str(r) ? yyjson_get_str(r) : "");
                names += (names.empty() ? "" : ", ") +
                         (it == users.end() ? std::string("?") : it->second.name);
                if (c.icon_url.empty() && it != users.end())
                {
                    c.icon_url = it->second.avatar;
                }
            }
            if (c.name.empty())
            {
                c.name = names;
            }
            if (*str(v, "icon"))
            {
                c.icon_url = "https://cdn.discordapp.com/channel-icons/" + c.id + "/" +
                             str(v, "icon") + ".png?size=64";
            }
            apply_read(c);
            dms.channels.push_back(c);
        }
        std::sort(dms.channels.begin(), dms.channels.end(), [](const Channel &a, const Channel &b)
                  { return newer(a.last_message_id, b.last_message_id); });
        refresh_guild(dms);

        std::vector<Guild> guilds;
        guilds.push_back(dms);
        yyjson_val *merged = yyjson_obj_get(d, "merged_members");
        size_t gi = 0;
        yyjson_arr_foreach(yyjson_obj_get(d, "guilds"), idx, max, v)
        {
            yyjson_val *props = yyjson_obj_get(v, "properties");
            auto field = [&](const char *key)
            {
                std::string value = sval(v, key);
                return value.empty() ? sval(props, key) : value;
            };
            Guild g;
            g.id = str(v, "id");
            g.name = field("name");
            const std::string icon = field("icon");
            if (!icon.empty())
            {
                g.icon_url =
                    "https://cdn.discordapp.com/icons/" + g.id + "/" + icon + ".png?size=128";
            }
            const std::string rules_channel = field("rules_channel_id");
            const bool owner = field("owner_id") == me_id;

            std::map<std::string, uint64_t> role_perms;
            size_t i2;
            size_t m2;
            yyjson_val *c;
            yyjson_arr_foreach(yyjson_obj_get(v, "roles"), i2, m2, c)
            {
                Role role;
                role.id = str(c, "id");
                role.name = str(c, "name");
                role.color = uint32_t(yyjson_get_int(yyjson_obj_get(c, "color")));
                role.position = int(yyjson_get_int(yyjson_obj_get(c, "position")));
                role_perms[role.id] = u64(sval(c, "permissions"));
                g.roles.push_back(role);
            }
            std::vector<std::string> my_roles;
            yyjson_arr_foreach(yyjson_arr_get(merged, gi), i2, m2, c)
            {
                if (sval(c, "user_id") == me_id)
                {
                    my_roles = string_list(yyjson_obj_get(c, "roles"));
                }
            }
            ++gi;
            member_roles_[g.id][me_id] = my_roles;
            uint64_t base = role_perms[g.id];
            for (const std::string &r : my_roles)
            {
                base |= role_perms[r];
            }
            const bool admin = owner || (base & 0x8);
            auto can_view = [&](yyjson_val *ch)
            {
                if (admin)
                {
                    return true;
                }
                uint64_t p = base;
                uint64_t role_allow = 0;
                uint64_t role_deny = 0;
                uint64_t member_allow = 0;
                uint64_t member_deny = 0;
                size_t i3;
                size_t m3;
                yyjson_val *o;
                yyjson_arr_foreach(yyjson_obj_get(ch, "permission_overwrites"), i3, m3, o)
                {
                    const std::string id = sval(o, "id");
                    const uint64_t allow = u64(sval(o, "allow"));
                    const uint64_t deny = u64(sval(o, "deny"));
                    if (id == g.id)
                    {
                        p = (p & ~deny) | allow;
                    }
                    else if (id == me_id)
                    {
                        member_allow |= allow;
                        member_deny |= deny;
                    }
                    else if (std::find(my_roles.begin(), my_roles.end(), id) != my_roles.end())
                    {
                        role_allow |= allow;
                        role_deny |= deny;
                    }
                }
                p = (p & ~role_deny) | role_allow;
                p = (p & ~member_deny) | member_allow;
                return (p & 0x400) != 0;
            };

            std::map<std::string, int> category_pos;
            yyjson_arr_foreach(yyjson_obj_get(v, "channels"), i2, m2, c)
            {
                if (yyjson_get_int(yyjson_obj_get(c, "type")) == 4)
                {
                    int pos = int(yyjson_get_int(yyjson_obj_get(c, "position")));
                    category_pos[str(c, "id")] = pos;
                    Channel category;
                    category.id = str(c, "id");
                    category.name = str(c, "name");
                    category.type = 4;
                    category.position = (pos + 1) * 1000 - 1;
                    g.channels.push_back(category);
                }
            }
            std::set<std::string> used_categories;
            yyjson_arr_foreach(yyjson_obj_get(v, "channels"), i2, m2, c)
            {
                int t = int(yyjson_get_int(yyjson_obj_get(c, "type")));
                if ((t != 0 && t != 2 && t != 5 && t != 13 && t != 15 && t != 16) || !can_view(c))
                {
                    continue;
                }
                Channel ch;
                ch.id = str(c, "id");
                ch.name = str(c, "name");
                ch.type = t;
                ch.topic = str(c, "topic");
                ch.last_message_id = sval(c, "last_message_id");
                ch.rules = ch.id == rules_channel;
                ch.parent_id = str(c, "parent_id");
                int cat = ch.parent_id.empty() ? -1 : category_pos[ch.parent_id];
                ch.position = (cat + 1) * 1000 +
                              int(yyjson_get_int(yyjson_obj_get(c, "position"))) +
                              (t == 2 || t == 13 ? 500 : 0);
                if (t == 0 || t == 5)
                {
                    apply_read(ch);
                }
                used_categories.insert(ch.parent_id);
                g.channels.push_back(ch);
            }
            g.channels.erase(
                std::remove_if(g.channels.begin(), g.channels.end(), [&](const Channel &ch)
                               { return ch.type == 4 && !used_categories.count(ch.id); }),
                g.channels.end());
            std::sort(g.channels.begin(), g.channels.end(),
                      [](const Channel &a, const Channel &b) { return a.position < b.position; });
            std::vector<std::string> in_voice;
            yyjson_arr_foreach(yyjson_obj_get(v, "voice_states"), i2, m2, c)
            {
                const std::string channel_id = str(c, "channel_id");
                for (Channel &ch : g.channels)
                {
                    if (ch.id == channel_id)
                    {
                        VoiceUser user;
                        user.id = str(c, "user_id");
                        user.muted = yyjson_get_bool(yyjson_obj_get(c, "self_mute")) ||
                                     yyjson_get_bool(yyjson_obj_get(c, "mute"));
                        user.deafened = yyjson_get_bool(yyjson_obj_get(c, "self_deaf")) ||
                                        yyjson_get_bool(yyjson_obj_get(c, "deaf"));
                        if (yyjson_val *who = yyjson_obj_get(yyjson_obj_get(c, "member"), "user"))
                        {
                            user.name = display_name(who);
                            user.avatar_url = avatar_url(who);
                        }
                        else
                        {
                            in_voice.push_back(user.id);
                        }
                        ch.voice_users.push_back(user);
                    }
                }
            }
            if (!in_voice.empty())
            {
                request_members(g.id, in_voice);
            }
            g.muted = muted_guilds.count(g.id) > 0;
            refresh_guild(g);
            std::printf("[voice] guild %s %s\n", g.id.c_str(), g.name.c_str());
            guilds.push_back(std::move(g));
        }
        update(
            [&](Snapshot &s)
            {
                s.state = State::ready;
                s.status.clear();
                s.me = me;
                s.me_id = me_id;
                s.me_avatar = me_avatar;
                me_id_ = me_id;
                s.guilds = std::move(guilds);
            });
        check_invite();
        post([this] { rejoin_voice(); });
    }
    else if (type == "MESSAGE_CREATE")
    {
        const std::string channel = str(d, "channel_id");
        const std::string guild_id = str(d, "guild_id");
        Message msg = parse_message(d);
        yyjson_val *member = yyjson_obj_get(d, "member");
        if (member && !guild_id.empty())
        {
            member_roles_[guild_id][msg.author_id] = string_list(yyjson_obj_get(member, "roles"));
        }
        const bool everyone = yyjson_get_bool(yyjson_obj_get(d, "mention_everyone"));
        bool open = false;
        std::string note;
        update(
            [&](Snapshot &s)
            {
                Guild *g = nullptr;
                Channel *c = find_channel(s, channel, &g);
                if (c)
                {
                    c->last_message_id = msg.id;
                }
                if (s.channel_id == channel)
                {
                    open = true;
                    s.messages.push_back(msg);
                    if (s.messages.size() > kMaxMessages)
                    {
                        s.messages.erase(s.messages.begin());
                    }
                    recolor(s);
                }
                else if (c && msg.author_id != s.me_id)
                {
                    bool me_mentioned = everyone || guild_id.empty();
                    for (const Mention &m : msg.mentions)
                    {
                        me_mentioned = me_mentioned || m.id == s.me_id;
                    }
                    c->unread = true;
                    if (me_mentioned)
                    {
                        c->mentions++;
                        const std::string who =
                            msg.author.empty() ? std::string("Discord") : msg.author;
                        note = guild_id.empty() ? i18n::T("%s mesaj gönderdi", who)
                                                : i18n::T("%s seni etiketledi", who);
                    }
                    refresh_guild(*g);
                }
                if (c && g->id.empty())
                {
                    auto it = std::find_if(g->channels.begin(), g->channels.end(),
                                           [&](const Channel &ch) { return ch.id == channel; });
                    std::rotate(g->channels.begin(), it, it + 1);
                }
            });
        if (open && msg.author_id != state_.me_id)
        {
            mark_read(channel, msg.id);
        }
        if (!note.empty())
        {
            notify(note);
        }
    }
    else if (type == "MESSAGE_UPDATE")
    {
        const std::string id = str(d, "id");
        const bool full = yyjson_is_obj(yyjson_obj_get(d, "author"));
        Message fresh = full ? parse_message(d) : Message{};
        std::vector<Embed> embeds;
        yyjson_arr_foreach(yyjson_obj_get(d, "embeds"), idx, max, v)
        {
            embeds.push_back(parse_embed(v));
        }
        update(
            [&](Snapshot &s)
            {
                for (Message &m : s.messages)
                {
                    if (m.id != id)
                    {
                        continue;
                    }
                    if (full)
                    {
                        fresh.reactions = m.reactions;
                        fresh.color = m.color;
                        m = fresh;
                    }
                    else if (yyjson_obj_get(d, "embeds"))
                    {
                        m.embeds = embeds;
                    }
                }
            });
    }
    else if (type == "MESSAGE_DELETE")
    {
        std::string id = str(d, "id");
        update(
            [&](Snapshot &s)
            {
                s.messages.erase(std::remove_if(s.messages.begin(), s.messages.end(),
                                                [&](const Message &m) { return m.id == id; }),
                                 s.messages.end());
            });
    }
    else if (type == "MESSAGE_REACTION_ADD" || type == "MESSAGE_REACTION_REMOVE")
    {
        const bool add = type == "MESSAGE_REACTION_ADD";
        const std::string message_id = str(d, "message_id");
        const std::string user_id = str(d, "user_id");
        yyjson_val *emoji = yyjson_obj_get(d, "emoji");
        const std::string name = str(emoji, "name");
        const std::string url = md::reaction_emoji_url(
            name, str(emoji, "id"), yyjson_get_bool(yyjson_obj_get(emoji, "animated")));
        update(
            [&](Snapshot &s)
            {
                for (Message &m : s.messages)
                {
                    if (m.id != message_id)
                    {
                        continue;
                    }
                    auto it = std::find_if(m.reactions.begin(), m.reactions.end(),
                                           [&](const Reaction &r) { return r.emoji_url == url; });
                    if (add)
                    {
                        if (it == m.reactions.end())
                        {
                            m.reactions.push_back({url, name, 0, false});
                            it = m.reactions.end() - 1;
                        }
                        it->count++;
                        it->me = it->me || user_id == s.me_id;
                    }
                    else if (it != m.reactions.end())
                    {
                        it->count--;
                        if (user_id == s.me_id)
                        {
                            it->me = false;
                        }
                        if (it->count <= 0)
                        {
                            m.reactions.erase(it);
                        }
                    }
                }
            });
    }
    else if (type == "MESSAGE_ACK")
    {
        const std::string channel = str(d, "channel_id");
        update(
            [&](Snapshot &s)
            {
                Guild *g = nullptr;
                if (Channel *c = find_channel(s, channel, &g))
                {
                    c->unread = false;
                    c->mentions = 0;
                    refresh_guild(*g);
                }
            });
    }
    else if (type == "GUILD_MEMBERS_CHUNK")
    {
        const std::string guild = str(d, "guild_id");
        std::map<std::string, VoiceUser> people;
        yyjson_arr_foreach(yyjson_obj_get(d, "members"), idx, max, v)
        {
            yyjson_val *user = yyjson_obj_get(v, "user");
            member_roles_[guild][str(user, "id")] = string_list(yyjson_obj_get(v, "roles"));
            VoiceUser &person = people[str(user, "id")];
            const char *nick = yyjson_get_str(yyjson_obj_get(v, "nick"));
            person.name = nick && *nick ? nick : display_name(user);
            person.avatar_url = avatar_url(user);
        }
        update(
            [&](Snapshot &s)
            {
                recolor(s);
                if (Guild *g = find_guild(s, guild))
                {
                    for (Channel &c : g->channels)
                    {
                        for (VoiceUser &u : c.voice_users)
                        {
                            auto it = people.find(u.id);
                            if (u.name.empty() && it != people.end())
                            {
                                u.name = it->second.name;
                                u.avatar_url = it->second.avatar_url;
                            }
                        }
                    }
                }
            });
    }
    else if (type == "GUILD_MEMBER_LIST_UPDATE")
    {
        apply_member_list(d);
    }
    else if (type == "VOICE_STATE_UPDATE")
    {
        std::printf("[voice] event state guild=%s channel=%s user=%s\n", str(d, "guild_id"),
                    str(d, "channel_id"), str(d, "user_id"));
        voice_state(d, str(d, "guild_id"));
    }
    else if (type == "VOICE_SERVER_UPDATE")
    {
        std::printf("[voice] event server guild=%s endpoint=%s have_token=%d wanted=%s\n",
                    str(d, "guild_id"), str(d, "endpoint"),
                    yyjson_get_str(yyjson_obj_get(d, "token")) ? 1 : 0, voice_guild_.c_str());
        if (str(d, "guild_id") == voice_guild_ && !voice_guild_.empty())
        {
            const std::string token = str(d, "token");
            const std::string endpoint = str(d, "endpoint");
            const bool changed = (!token.empty() && token != voice_token_) ||
                                 (!endpoint.empty() && endpoint != voice_endpoint_);
            if (!token.empty())
            {
                voice_token_ = token;
            }
            if (!endpoint.empty())
            {
                voice_endpoint_ = endpoint;
            }
            if (changed)
            {
                voice_armed_session_.clear();
            }
            std::printf("[voice] server endpoint=%s token=%s\n", voice_endpoint_.c_str(),
                        voice_token_.empty() ? "no" : "yes");
            maybe_start_voice();
        }
    }
}

// The posts of a forum, newest first. Discord answers the search endpoint with
// the opening message of each one, which is what the list shows under the title.
void Client::load_posts(const std::string &forum_id)
{
    update(
        [&](Snapshot &s)
        {
            Guild *g = nullptr;
            const Channel *forum = find_channel(s, forum_id, &g);
            s.guild_id = g ? g->id : std::string();
            s.channel_id = forum_id;
            s.channel_error.clear();
            s.forum_id.clear();
            s.forum_name.clear();
            s.messages.clear();
            s.loading_posts = forum == nullptr || forum->posts.empty();
        });
    net::Response r =
        net::request("GET",
                     std::string(kApi) + "/channels/" + forum_id +
                         "/threads/search?archived=false&sort_by=last_message_time&sort_order=desc"
                         "&limit=25",
                     api_headers(token_));
    if (r.status != 200)
    {
        // Older servers and bots-only tokens answer this one instead.
        r = net::request("GET",
                         std::string(kApi) + "/channels/" + forum_id +
                             "/threads/archived/public?limit=25",
                         api_headers(token_));
    }
    std::vector<ForumPost> posts;
    std::string error;
    if (r.status == 200)
    {
        yyjson_doc *doc = yyjson_read(r.body.data(), r.body.size(), 0);
        yyjson_val *root = yyjson_doc_get_root(doc);
        size_t idx;
        size_t max;
        yyjson_val *v;
        // The opening messages come in a sibling array; each names its thread.
        std::map<std::string, std::pair<std::string, std::string>>
            openers; // thread -> author, text
        yyjson_arr_foreach(yyjson_obj_get(root, "first_messages"), idx, max, v)
        {
            openers[str(v, "channel_id")] = {display_name(yyjson_obj_get(v, "author")),
                                             str(v, "content")};
        }
        yyjson_arr_foreach(yyjson_obj_get(root, "threads"), idx, max, v)
        {
            ForumPost p;
            p.id = str(v, "id");
            p.name = str(v, "name");
            p.message_count = int(yyjson_get_int(yyjson_obj_get(v, "message_count")));
            yyjson_val *meta = yyjson_obj_get(v, "thread_metadata");
            p.timestamp = parse_iso(str(meta, "create_timestamp"));
            if (const auto it = openers.find(p.id); it != openers.end())
            {
                p.author = it->second.first;
                p.excerpt = it->second.second;
            }
            posts.push_back(std::move(p));
        }
        yyjson_doc_free(doc);
    }
    else if (r.status == 403)
    {
        error = i18n::T("Bu kanalı görme iznin yok");
    }
    else
    {
        error = i18n::T("Mesajlar yüklenemedi (%s)", r.status ? std::to_string(r.status) : r.error);
    }
    update(
        [&](Snapshot &s)
        {
            if (s.channel_id != forum_id)
            {
                return;
            }
            s.loading_posts = false;
            s.channel_error = error;
            if (Channel *forum = find_channel(s, forum_id))
            {
                forum->posts = std::move(posts);
            }
        });
}

void Client::load_messages(const std::string &channel_id, const std::string &forum_id)
{
    std::string guild_id;
    std::string forum_name;
    update(
        [&](Snapshot &s)
        {
            Guild *g = nullptr;
            find_channel(s, forum_id.empty() ? channel_id : forum_id, &g);
            guild_id = g ? g->id : std::string();
            if (s.guild_id != guild_id)
            {
                s.members.clear();
            }
            if (const Channel *forum = forum_id.empty() ? nullptr : find_channel(s, forum_id))
            {
                forum_name = forum->name;
            }
            s.guild_id = guild_id;
            s.channel_id = channel_id;
            s.channel_error.clear();
            s.forum_id = forum_id;
            s.forum_name = forum_name;
            s.messages.clear();
            s.loading_messages = true;
            s.loading_posts = false;
        });
    if (!guild_id.empty())
    {
        subscribe_member_list(guild_id, forum_id.empty() ? channel_id : forum_id);
    }
    net::Response r =
        net::request("GET", std::string(kApi) + "/channels/" + channel_id + "/messages?limit=50",
                     api_headers(token_));
    std::vector<Message> messages;
    std::string error;
    if (r.status == 200)
    {
        yyjson_doc *doc = yyjson_read(r.body.data(), r.body.size(), 0);
        size_t idx;
        size_t max;
        yyjson_val *m;
        yyjson_arr_foreach(yyjson_doc_get_root(doc), idx, max, m)
        {
            messages.push_back(parse_message(m));
        }
        yyjson_doc_free(doc);
        std::reverse(messages.begin(), messages.end());
    }
    else if (r.status == 403)
    {
        error = i18n::T("Bu kanalı görme iznin yok");
    }
    else
    {
        error = i18n::T("Mesajlar yüklenemedi (%s)", r.status ? std::to_string(r.status) : r.error);
    }
    std::vector<std::string> authors;
    for (const Message &m : messages)
    {
        if (!guild_id.empty() && !member_roles_[guild_id].count(m.author_id))
        {
            authors.push_back(m.author_id);
        }
    }
    const std::string last = messages.empty() ? std::string() : messages.back().id;
    update(
        [&](Snapshot &s)
        {
            if (s.channel_id != channel_id)
            {
                return;
            }
            s.loading_messages = false;
            s.channel_error = error;
            s.messages = std::move(messages);
            recolor(s);
        });
    if (!guild_id.empty())
    {
        request_members(guild_id, authors);
    }
    mark_read(channel_id, last);
}

} // namespace dc
