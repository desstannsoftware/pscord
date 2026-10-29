// ps5-discord - Offline stand-ins for dc::Client and dc::Media so the PC snapshots
// can draw the Discord screen with sample servers and messages.
// DC_PREVIEW=login shows sign-in. DC_MEDIA_DIR holds downloaded pictures, named by
// the FNV-1a hash of their URL; URLs not found there are listed in missing.txt.
// Copyright (C) 2026 Desstann
// SPDX-License-Identifier: MIT

#include "dc/client.hpp"
#include "dc/markdown.hpp"
#include "dc/media.hpp"
#include "update/updater.hpp"
#include "voice/bg_client.hpp"
#include "voice/engine.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>

namespace dc {

namespace {

std::string avatar(int n) {
    return "https://cdn.discordapp.com/embed/avatars/" + std::to_string(n) + ".png";
}

Channel channel(const char* id, const char* name, int type = 0, const char* topic = "") {
    Channel c;
    c.id = id;
    c.name = name;
    c.type = type;
    c.topic = topic;
    return c;
}

Message message(const char* author, int avatar_index, const char* content, int64_t minutes_ago) {
    Message m;
    static int next = 0;
    m.id = "m" + std::to_string(++next);
    m.author_id = author;
    m.author = author;
    m.avatar_url = avatar(avatar_index);
    m.content = content;
    m.timestamp = int64_t(std::time(nullptr)) - minutes_ago * 60;
    return m;
}

Snapshot sample() {
    Snapshot s;
    const char* mode = std::getenv("DC_PREVIEW");
    if (mode && std::strcmp(mode, "login") == 0) {
        s.state = State::login;
        s.qr_url = "https://discord.com/ra/preview-preview-preview-preview";
        s.web_url = "http://192.168.1.100:8090";
        s.status = "Giriş bekleniyor";
        return s;
    }
    s.state = State::ready;
    s.me = "desstann";
    s.me_id = "desstann";
    s.me_avatar = avatar(4);

    Guild dms;
    dms.name = "Özel Mesajlar";
    dms.channels = {channel("d1", "Mert", 1), channel("d2", "Elif", 1), channel("d3", "Oyun Ekibi", 3)};
    dms.channels[0].icon_url = avatar(0);
    dms.channels[1].icon_url = avatar(3);
    dms.channels[0].unread = true;
    dms.channels[0].mentions = 2;
    dms.mentions = 2;

    Guild home;
    home.id = "g1";
    home.name = "Desstann Homebrew";
    home.roles = {{"r1", "Desstann", 0x9b84ee, 10}, {"r2", "Moderatör", 0x3ba55c, 5}};
    home.channels = {
        channel("k1", "📌 START HERE", 4),
        channel("1001", "welcome", 0, "Start here: apps and links"),
        channel("1002", "rules", 0),
        channel("1003", "announcements", 5),
        channel("1004", "releases", 5),
        channel("1005", "buy-me-a-coffee", 0),
        channel("k2", "💬 COMMUNITY", 4),
        channel("1006", "general", 0),
        channel("1007", "showcase", 0),
        channel("1008", "suggestions", 15),
        channel("1009", "off-topic", 0),
        channel("k3", "🧰 APP SUPPORT", 4),
        channel("1010", "how-to-get-help", 0),
        channel("1011", "pkg-folderizer", 15),
        channel("k4", "🔒 DEV", 4),
        channel("1012", "mod-updates", 0),
        channel("1013", "bug-triage", 0),
        channel("k5", "🔊 VOICE", 4),
        channel("1014", "Genel", 2),
    };
    home.channels.back().voice_users = {{"desstann", "desstann", avatar(4)},
                                        {"Elif", "Elif", avatar(3)},
                                        {"Mert", "Mert", avatar(0), true},
                                        {"andy", "andy", avatar(1)},
                                        {"SMALL ARIAN", "SMALL ARIAN", avatar(2), true, true}};
    s.voice_guild_id = "g1";
    s.voice_channel_id = "1014";
    home.channels[2].rules = true;
    home.channels[4].unread = true;
    home.channels[7].unread = true;
    home.channels[7].mentions = 1;
    home.unread = true;
    home.mentions = 1;

    Guild games;
    games.id = "g2";
    games.name = "Gece Oyuncuları";
    games.icon_url = avatar(2);
    games.channels = {channel("e1", "genel", 0)};
    games.unread = true;
    Guild art;
    art.id = "g3";
    art.name = "Pixel Art 🎨";
    art.channels = {channel("f1", "galeri", 0)};
    art.mentions = 12;
    s.guilds = {dms, home, games, art};
    return s;
}

std::vector<Message> sample_messages() {
    Message welcome = message("DesstannHB", 1,
                              "# Welcome to Desstann Homebrew 👋\n"
                              "Home of PS5 homebrew apps by **desstann**.\n\n"
                              "## 🧰 Apps\n"
                              "**PKG Folderizer**: converts debug/fake `.pkg` files into ShadowMount folders, "
                              "`.ffpfsc` or `.exfat` images on the console.\n"
                              "Download: <https://desstannsoftware.com/#pkg-folderizer>\n"
                              "Support: <#1011>\n\n"
                              "## ❓ Need help?\n"
                              "Read <#1010>, then open a post in your app's support forum.\n"
                              "New releases land in <#1004>, news in <#1003>.\n\n"
                              "## ☕ Support the work\n"
                              "The apps are free. If they saved you time, you can buy me a coffee: <#1005> -# (sizlerindi)",
                              60 * 26);
    welcome.bot = true;
    welcome.author_id = "bot";
    welcome.reactions = {{md::reaction_emoji_url("👍", "", false), "👍", 12, true},
                         {md::reaction_emoji_url("🔥", "", false), "🔥", 5, false},
                         {md::reaction_emoji_url("❤️", "", false), "❤️", 3, false}};

    Message photo = message("Mert", 0, "13.60'ta etaHEN ile açtım, ekran görüntüsü 😄", 40);
    Attachment shot;
    shot.filename = "ps5-home.jpg";
    shot.media.url = "https://picsum.photos/id/1015/660/400.jpg";
    shot.media.width = 660;
    shot.media.height = 400;
    photo.attachments.push_back(shot);

    Message gif = message("Mert", 0, "https://giphy.com/gifs/JIX9t2j0ZTN9S", 39);
    Embed gif_embed;
    gif_embed.type = "gifv";
    gif_embed.url = "https://giphy.com/gifs/JIX9t2j0ZTN9S";
    gif_embed.image.url = "https://media.giphy.com/media/JIX9t2j0ZTN9S/giphy.gif";
    gif_embed.image.width = 360;
    gif_embed.image.height = 270;
    gif.embeds.push_back(gif_embed);

    Message reply = message("Elif", 3, "Süper! **Kernel exploit** sorunsuz mu? ||spoiler yok||", 12);
    reply.reply = true;
    reply.reply_author = "Mert";
    reply.reply_author_id = "Mert";
    reply.reply_avatar = avatar(0);
    reply.reply_content = "13.60'ta etaHEN ile açtım, ekran görüntüsü 😄";
    reply.color = 0x3ba55c;

    Message link = message("desstann", 4,
                           "Yeni sürüm çıktı 🎉 ayrıntılar: https://desstannsoftware.com\n"
                           "> Klasörleme artık 2 kat hızlı\n"
                           "- exFAT desteği\n- `.ffpfsc` imajları\n"
                           "```\nmake && make deploy\n```",
                           3);
    link.color = 0x9b84ee;
    Embed card;
    card.type = "rich";
    card.url = "https://desstannsoftware.com";
    card.provider = "GitHub";
    card.title = "desstann/ps5-pkg-folderizer";
    card.description = "Converts debug/fake PKG files into ShadowMount folders on the console.";
    card.color = 0x24292f;
    card.has_color = true;
    link.embeds.push_back(card);

    Message join = message("Ben M", 2, "", 2);
    join.type = 7;
    Message emoji = message("Elif", 3, "🔥🔥🔥", 1);
    emoji.color = 0x3ba55c;
    return {welcome, photo, gif, reply, link, join, emoji};
}

std::vector<Member> sample_members() {
    auto member = [](const char* name, int av, const char* status, uint32_t color, bool bot = false) {
        Member m;
        m.id = name;
        m.label = name;
        m.avatar_url = avatar(av);
        m.status = status;
        m.color = color;
        m.bot = bot;
        return m;
    };
    auto group = [](const char* label, int count) {
        Member m;
        m.group = true;
        m.label = label;
        m.count = count;
        return m;
    };
    return {group("Desstann", 1),  member("desstann", 4, "online", 0x9b84ee), group("Çevrim içi", 2),
            member("SMALL ARIAN", 5, "dnd", 0), member("Elif", 3, "idle", 0x3ba55c), group("Çevrim dışı", 3),
            member("andy", 1, "offline", 0), member("Ben M", 2, "offline", 0),
            member("DesstannHB", 1, "offline", 0, true)};
}

uint32_t fnv(const std::string& s) {
    uint32_t h = 2166136261u;
    for (unsigned char c : s) {
        h = (h ^ c) * 16777619u;
    }
    return h;
}

} // namespace

Client& Client::instance() {
    static Client client;
    return client;
}

void Client::start(const std::string&, const std::string&, const std::string&) {}

void Client::start_web(uint16_t) {}

bool Client::snapshot_if_newer(uint64_t revision, Snapshot& out) {
    std::lock_guard<std::mutex> hold(lock_);
    if (state_.revision == 0) {
        state_ = sample();
        state_.revision = 1;
        // PREVIEW_INVITE=1 offers the community server; =failed shows its QR code.
        if (const char* invite = std::getenv("PREVIEW_INVITE")) {
            state_.invite.code = "pscord";
            state_.invite.guild_name = "PSCord Topluluğu";
            state_.invite.members = 1284;
            state_.invite.online = 312;
            state_.invite.join = std::strcmp(invite, "failed") == 0 ? Invite::Join::failed : Invite::Join::idle;
        }
    }
    if (state_.revision == revision) {
        return false;
    }
    out = state_;
    return true;
}

void Client::open_channel(const std::string& channel_id) {
    update([&](Snapshot& s) {
        s.channel_id = channel_id;
        s.guild_id = channel_id.rfind("10", 0) == 0 ? "g1" : "";
        s.messages = channel_id.rfind("10", 0) == 0 ? sample_messages() : std::vector<Message>{};
        s.members = channel_id.rfind("10", 0) == 0 ? sample_members() : std::vector<Member>{};
    });
}

void Client::send_message(const std::string& text) {
    update([&](Snapshot& s) { s.messages.push_back(message(s.me.c_str(), 4, text.c_str(), 0)); });
}

void Client::logout() {}

void Client::retry_connect() {}

void Client::join_invite() {
    update([](Snapshot& s) { s.invite.join = Invite::Join::joined; });
}

void Client::dismiss_invite() {
    update([](Snapshot& s) { s.invite = Invite{}; });
}

void Client::join_voice(const std::string& guild_id, const std::string& channel_id) {
    update([&](Snapshot& s) {
        s.voice_guild_id = guild_id;
        s.voice_channel_id = channel_id;
    });
}

void Client::set_voice_muted(bool muted) {
    update([&](Snapshot& s) { s.voice_muted = muted; });
}

void Client::leave_voice() {
    update([](Snapshot& s) {
        s.voice_guild_id.clear();
        s.voice_channel_id.clear();
    });
}

Media& Media::instance() {
    static Media media;
    return media;
}

void Media::request(const std::string& url, int max_w, int max_h) {
    const char* dir = std::getenv("DC_MEDIA_DIR");
    if (!dir || url.empty() || !seen_.insert(url).second) {
        return;
    }
    char name[16];
    std::snprintf(name, sizeof(name), "%08x", fnv(url));
    std::ifstream file(std::string(dir) + "/" + name, std::ios::binary);
    Picture picture;
    picture.url = url;
    if (!file) {
        std::ofstream(std::string(dir) + "/missing.txt", std::ios::app) << name << " " << url << "\n";
    } else {
        std::stringstream bytes;
        bytes << file.rdbuf();
        if (!decode_picture(bytes.str(), max_w, max_h, picture)) {
            picture.width = 0;
            picture.frames.clear();
        }
    }
    done_.push_back(std::move(picture));
}

bool Media::take(Picture& out) {
    if (done_.empty()) {
        return false;
    }
    out = std::move(done_.front());
    done_.pop_front();
    return true;
}

void Media::forget(const std::string& url) {
    seen_.erase(url);
}

} // namespace dc

namespace voice {

// The preview is in a call where Elif is talking.
struct Engine::Impl {};

Engine& Engine::instance() {
    static Engine engine;
    return engine;
}

Engine::Engine() = default;
Engine::~Engine() = default;

void Engine::set_output(hui::audio::StreamRing*) {}
void Engine::start(const JoinInfo&) {}
void Engine::stop() {}
void Engine::set_muted(bool) {}

Status Engine::status() const {
    Status status;
    status.phase = Phase::connected;
    status.e2ee = true;
    status.speaking = {"Elif"};
    status.mic = true;
    return status;
}

namespace bg {

bool available() { return false; }
bool ensure_running(const std::string&, std::string* status) {
    if (status) *status = "preview";
    return false;
}
bool join(const JoinInfo&, bool, std::string*) { return false; }
bool leave(std::string*) { return false; }
bool set_muted(bool, std::string*) { return false; }
std::string status_line() { return {}; }
Status live(const Status& local) { return local; }

} // namespace bg

} // namespace voice

namespace update {

// PREVIEW_UPDATE=1 shows an update half downloaded; otherwise there is none.
Updater& Updater::instance() {
    static Updater updater;
    return updater;
}

Updater::~Updater() = default;
void Updater::retry() {}

Status Updater::status() const {
    Status status;
    status.phase = Phase::downloading;
    status.version = "01.001.000";
    status.notes = "Mikrofon desteği ve yeni ikon";
    status.received = 23u << 20;
    status.total = 45u << 20;
    return status;
}

bool Updater::blocking() const {
    return std::getenv("PREVIEW_UPDATE") != nullptr;
}

} // namespace update
