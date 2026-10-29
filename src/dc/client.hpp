// ps5-discord - Discord session: QR login, gateway, channels and messages.
// Copyright (C) 2026 Desstann
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <map>
#include <set>
#include <mutex>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "dc/net/http_server.hpp"

namespace dc
{

// Someone connected to a voice channel.
struct VoiceUser
{
    std::string id;
    std::string name;
    std::string avatar_url;
    bool muted = false;
    bool deafened = false;
};

// A post in a forum or media channel. Its id is also the channel its replies
// live in, so opening one reads messages like any other channel.
struct ForumPost
{
    std::string id;
    std::string name;
    std::string author;
    std::string excerpt; // the opening message, one line
    int message_count = 0;
    int64_t timestamp = 0; // when it was posted
};

struct Channel
{
    std::string id;
    std::string name;
    // 0 text, 1 DM, 2 voice, 3 group DM, 4 category, 5 announcement, 13 stage, 15 forum, 16 media
    int type = 0;
    int position = 0;
    std::string parent_id;
    std::string topic;
    std::string icon_url; // DMs: the other person's avatar
    std::string last_message_id;
    bool rules = false;
    bool unread = false;
    bool muted = false;
    int mentions = 0;
    std::vector<VoiceUser> voice_users; // voice and stage channels
    std::vector<ForumPost> posts;       // forum and media channels
};

struct Role
{
    std::string id;
    std::string name;
    uint32_t color = 0; // 0: no colour
    int position = 0;
};

struct Guild
{
    std::string id; // empty for the direct-messages entry
    std::string name;
    std::string icon_url;
    std::vector<Channel> channels;
    std::vector<Role> roles;
    bool unread = false;
    bool muted = false;
    int mentions = 0;
};

struct MediaRef
{
    std::string url; // what to fetch (already sized and in a format we decode)
    int width = 0;
    int height = 0;
    bool video = false; // a still of a video: draw a play button over it
};

struct Attachment
{
    std::string filename;
    std::string url;
    uint64_t size = 0;
    MediaRef media; // empty url for files that are not pictures or videos
};

struct EmbedField
{
    std::string name;
    std::string value;
    bool is_inline = false;
};

struct Embed
{
    std::string type; // rich, image, gifv, video, link, article
    std::string url;
    std::string title;
    std::string description;
    std::string provider;
    std::string author;
    std::string author_icon;
    std::string footer;
    uint32_t color = 0;
    bool has_color = false;
    MediaRef image;
    MediaRef thumbnail;
    std::vector<EmbedField> fields;
};

struct Reaction
{
    std::string emoji_url;
    std::string name;
    int count = 0;
    bool me = false;
};

struct Sticker
{
    std::string name;
    std::string url; // empty for Lottie stickers, which we cannot draw
};

struct Mention
{
    std::string id;
    std::string name;
};

struct Message
{
    std::string id;
    int type = 0;
    std::string author_id;
    std::string author;
    std::string avatar_url;
    bool bot = false;
    uint32_t color = 0; // the author's role colour, 0 for none
    std::string content;
    std::string time;      // HH:MM, local to the message's timestamp
    int64_t timestamp = 0; // unix seconds
    bool edited = false;
    std::vector<Mention> mentions;
    std::vector<Attachment> attachments;
    std::vector<Embed> embeds;
    std::vector<Reaction> reactions;
    std::vector<Sticker> stickers;
    bool reply = false;
    std::string reply_author_id;
    std::string reply_author;
    std::string reply_avatar;
    std::string reply_content;
    uint32_t reply_color = 0;
};

// One row of the member list: a group heading or a member.
struct Member
{
    bool group = false;
    std::string label; // group name, or the member's name
    int count = 0;
    std::string id;
    std::string avatar_url;
    uint32_t color = 0;
    std::string status; // online, idle, dnd, offline
    bool bot = false;
};

enum class State
{
    starting,
    login,
    connecting,
    ready,
    failed
};

// Our community server, offered now and then to someone who is not in it.
struct Invite
{
    enum class Join
    {
        idle,
        joining,
        joined,
        failed
    };
    std::string code; // empty: nothing to offer
    std::string guild_name;
    std::string icon_url;
    int members = 0;
    int online = 0;
    Join join = Join::idle;
};

struct Snapshot
{
    uint64_t revision = 0;
    State state = State::starting;
    std::string status;       // one line for the user
    std::string qr_url;       // State::login: what the QR code holds
    std::string pending_user; // State::login: who scanned it, waiting for approval
    std::string web_url;      // State::login: the sign-in page a PC can open, if it runs
    std::string web_error;    // why the sign-in page is not running
    std::string me;
    std::string me_id;
    std::string me_avatar;
    std::vector<Guild> guilds;
    std::string guild_id; // of the open channel; empty for DMs
    std::string channel_id;
    std::string channel_error;
    // Set while a forum post is open: the forum it was opened from.
    std::string forum_id;
    std::string forum_name;
    bool loading_messages = false;
    bool loading_posts = false;
    std::vector<Message> messages;
    std::vector<Member> members;
    std::string voice_guild_id; // the call we are in, if any
    std::string voice_channel_id;
    bool voice_muted = false;
    Invite invite;
};

class Client
{
  public:
    static Client &instance();

    // ca_file: PEM bundle for TLS; session_file: where the token is kept;
    // import_file: a token dropped there (over FTP) signs in once, then is deleted.
    void start(const std::string &ca_file, const std::string &session_file,
               const std::string &import_file);

    // Copies the state into out when it changed since `revision`; true if it did.
    bool snapshot_if_newer(uint64_t revision, Snapshot &out);

    // Serves the sign-in page on this port for a PC on the same network.
    void start_web(uint16_t port);
    void open_channel(const std::string &channel_id);
    // Reads one post of a forum channel; open_channel(forum_id) goes back to the list.
    void open_post(const std::string &forum_id, const std::string &post_id);
    void send_message(const std::string &text);
    void join_voice(const std::string &guild_id, const std::string &channel_id);
    void leave_voice();
    void set_voice_muted(bool muted);
    void logout();
    // Closes a stuck gateway wait and connects again (loading / failed screen).
    void retry_connect();
    // After the first sign-in of every kInviteEvery-th launch, the invite in
    // config_url ({"invite": "discord.gg/..."}) is offered unless we are in that
    // server. counter_file counts the launches.
    void set_invite_source(const std::string &config_url, const std::string &counter_file);
    void join_invite();
    void dismiss_invite();
    // Opens the community invite card from Settings, ignoring the launch counter.
    void show_community_invite();

  private:
    Client() = default;
    void run();
    bool login_with_qr();
    void run_gateway();
    net::HttpReply web_api(const std::string &method, const std::string &path,
                           const std::string &body);
    std::string web_login(const std::string &path, const std::string &body);
    bool take_pending_token();
    void process_commands();
    void load_messages(const std::string &channel_id, const std::string &forum_id = {});
    void load_posts(const std::string &forum_id);
    void handle_dispatch(const std::string &type, const void *data);
    void gateway_send(const std::string &text);
    void request_members(const std::string &guild_id, const std::vector<std::string> &user_ids);
    void subscribe_member_list(const std::string &guild_id, const std::string &channel_id);
    void apply_member_list(const void *data);
    void recolor(Snapshot &s);
    void mark_read(const std::string &channel_id, const std::string &message_id);
    void voice_state(const void *state, const std::string &guild_id);
    void maybe_start_voice();
    void voice_watchdog();
    void check_invite();
    void offer_invite(bool skip_if_member);
    void remember_voice();
    void load_remembered_voice();
    void rejoin_voice();
    void notify(const std::string &text);

    template <typename F> void update(F &&change)
    {
        std::lock_guard<std::mutex> hold(lock_);
        const std::string before = state_.status;
        change(state_);
        state_.revision++;
        if (state_.status != before && !state_.status.empty())
        {
            std::printf("[dc] %s\n", state_.status.c_str());
        }
    }
    void post(std::function<void()> command);

    std::mutex lock_;
    Snapshot state_;
    std::deque<std::function<void()>> commands_;
    std::thread worker_;
    std::string session_file_;
    std::string import_file_;
    net::HttpServer web_;
    std::string pending_token_; // guarded by lock_
    std::string fingerprint_;   // web server thread only
    std::string token_;
    bool logout_requested_ = false;
    bool reconnect_now_ = false; // skip the pause between gateway attempts
    // Worker thread only.
    void *gateway_ = nullptr; // net::WebSocket while the gateway runs
    std::map<std::string, std::map<std::string, std::vector<std::string>>>
        member_roles_;                    // guild -> user -> roles
    std::set<std::string> asked_members_; // guild:user already requested
    std::vector<Member> member_items_;
    std::string voice_guild_; // the call asked for or joined
    std::string voice_channel_;
    std::string voice_session_;
    std::string voice_token_;
    std::string voice_endpoint_;
    // Session already handed to the voice engine. A move inside the same guild
    // keeps the token and only changes this once the new session id arrives.
    std::string voice_armed_session_;
    std::chrono::steady_clock::time_point voice_wait_since_{}; // op 4 sent, engine not armed yet
    bool voice_wait_retried_ = false;
    bool voice_muted_ = false;
    std::string me_id_;
    std::string invite_config_;
    std::string invite_counter_;
    std::string voice_file_;
    bool invite_checked_ = false;
};

} // namespace dc
