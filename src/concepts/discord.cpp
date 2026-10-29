// PS5 Discord - The one screen: sign-in, then servers, channels, messages and
// members laid out like the Discord desktop client, scaled 1.5x for a TV.
// Copyright (C) 2026 Desstann
// SPDX-License-Identifier: GPL-3.0-or-later

#include "concepts/concepts.hpp"

#include "concepts/discord_ui.hpp"
#include "dc/client.hpp"
#include "core/version.hpp"
#include "i18n/i18n.hpp"
#include "update/updater.hpp"
#include "voice/bg_client.hpp"
#include "voice/engine.hpp"
#include "third_party/qrcodegen/qrcodegen.h"
#include "ui/components.hpp"
#include "ui/glyphs.hpp"
#include "ui/widgets.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

using i18n::T;

namespace hui::concepts
{

namespace
{

using gfx::Color;
using gfx::Rect;
namespace palette = discord::palette;
using discord::Images;
using discord::RichText;

const Color kAvatarColors[] = {Color::rgb(0x5865f2), Color::rgb(0x757e8a), Color::rgb(0x3ba55c),
                               Color::rgb(0xfaa61a), Color::rgb(0xed4245), Color::rgb(0xeb459e)};

// Desktop Discord's geometry times 1.5.
namespace layout
{
constexpr float rail_w = 108;
constexpr float sidebar_x = rail_w;
constexpr float sidebar_w = 360;
constexpr float chat_x = sidebar_x + sidebar_w;
constexpr float members_w = 360;
constexpr float header_h = 72;
constexpr float panel_h = 80;
constexpr float voice_panel_h = 104; // the connected-call panel above the user panel
constexpr float voice_user_h = 40;
constexpr float icon = 72;
constexpr float icon_gap = 12;
constexpr float row_h = 48;
constexpr float dm_row_h = 66;
constexpr float category_h = 56;
constexpr float avatar = 60;
constexpr float composer_h = 66;
constexpr float composer_y = 1080 - 24 - composer_h;
constexpr float text_x = chat_x + 108;
constexpr float avatar_cx = chat_x + 24 + avatar * 0.5f;
} // namespace layout

constexpr const char *kTechniques[] = {
    "Sign-in page served to a PC on the same network; hCaptcha solved in its browser",
    "Discord remote-auth QR login: RSA-OAEP key pair, nonce proof, encrypted token",
    "TLS ClientHello split in two records so SNI filtering cannot read discord.com",
    "DNS over non-standard ports that the ISP does not intercept",
};

enum class Column
{
    guilds,
    channels,
    messages,
};

std::size_t utf8_length(unsigned char lead)
{
    return lead < 0x80 ? 1 : lead < 0xe0 ? 2 : lead < 0xf0 ? 3 : 4;
}

void pop_utf8(std::string &text)
{
    while (!text.empty())
    {
        const unsigned char c = static_cast<unsigned char>(text.back());
        text.pop_back();
        if ((c & 0xc0) != 0x80)
            break;
    }
}

// ui::upper only knows ASCII; channel categories are often Turkish.
std::string upper_tr(const std::string &name)
{
    static const char *const kPairs[][2] = {{"ç", "Ç"}, {"ğ", "Ğ"}, {"ı", "I"}, {"i", "İ"},
                                            {"ö", "Ö"}, {"ş", "Ş"}, {"ü", "Ü"}};
    std::string out;
    for (std::size_t i = 0; i < name.size();)
    {
        const std::size_t n =
            std::min(utf8_length(static_cast<unsigned char>(name[i])), name.size() - i);
        const std::string letter = name.substr(i, n);
        std::string mapped = ui::upper(letter);
        for (const auto &pair : kPairs)
            if (letter == pair[0])
                mapped = pair[1];
        out += mapped;
        i += n;
    }
    return out;
}

// The first letter of up to three words, as Discord labels a server without an icon.
std::string initials(const std::string &name)
{
    std::string out;
    int words = 0;
    bool at_start = true;
    for (std::size_t i = 0; i < name.size() && words < 3;)
    {
        const unsigned char c = static_cast<unsigned char>(name[i]);
        const std::size_t n = std::min(utf8_length(c), name.size() - i);
        if (c == ' ')
            at_start = true;
        else if (at_start)
        {
            // Emoji and symbols make poor initials.
            if (c < 0xf0)
            {
                out.append(name, i, n);
                ++words;
            }
            at_start = false;
        }
        i += n;
    }
    return out.empty() ? std::string("?") : out;
}

std::string first_letter(const std::string &name)
{
    if (name.empty())
        return "?";
    return upper_tr(name.substr(0, utf8_length(static_cast<unsigned char>(name[0]))));
}

Color avatar_color(const std::string &key)
{
    unsigned hash = 2166136261u;
    for (unsigned char c : key)
        hash = (hash ^ c) * 16777619u;
    return kAvatarColors[hash % (sizeof(kAvatarColors) / sizeof(kAvatarColors[0]))];
}

std::string file_size(std::uint64_t bytes)
{
    char buf[32];
    if (bytes >= 1024u * 1024u)
        std::snprintf(buf, sizeof(buf), "%.2f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    else
        std::snprintf(buf, sizeof(buf), "%.2f KB", static_cast<double>(bytes) / 1024.0);
    return buf;
}

int day_key(std::int64_t ts)
{
    const std::time_t t = static_cast<std::time_t>(ts);
    std::tm local{};
    localtime_r(&t, &local);
    return (local.tm_year + 1900) * 1000 + local.tm_yday;
}

std::string time_label(std::int64_t ts)
{
    const std::time_t t = static_cast<std::time_t>(ts);
    std::tm local{};
    localtime_r(&t, &local);
    const int day = day_key(ts);
    const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
    char buf[48];
    if (day == day_key(now))
        std::snprintf(buf, sizeof(buf), "%02d:%02d", local.tm_hour, local.tm_min);
    else if (day == day_key(now - 86400))
        std::snprintf(buf, sizeof(buf), T("Dün saat %02d:%02d"), local.tm_hour, local.tm_min);
    else
        std::snprintf(buf, sizeof(buf), "%02d.%02d.%04d %02d:%02d", local.tm_mday, local.tm_mon + 1,
                      local.tm_year + 1900, local.tm_hour, local.tm_min);
    return buf;
}

std::string day_label(std::int64_t ts)
{
    const std::time_t t = static_cast<std::time_t>(ts);
    std::tm local{};
    localtime_r(&t, &local);
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%d %s %d", local.tm_mday, i18n::month_name(local.tm_mon),
                  local.tm_year + 1900);
    return buf;
}

bool is_system(int type)
{
    return type != 0 && type != 19 && type != 20 && type != 23;
}

bool is_dm(const dc::Channel &c)
{
    return c.type == 1 || c.type == 3;
}

bool is_voice(const dc::Channel &c)
{
    return c.type == 2 || c.type == 13;
}

bool is_forum(const dc::Channel &c)
{
    return c.type == 15 || c.type == 16;
}

// The keyboard in Discord's colours: flat keys, blurple for the main action.
ui::Theme keyboard_theme()
{
    ui::Theme theme = ui::themes()[0];
    theme.style = ui::SurfaceStyle::flat;
    theme.page = palette::rail;
    theme.surface = palette::sidebar;
    theme.surface_high = palette::input;
    theme.text = palette::normal;
    theme.text_muted = palette::muted;
    theme.primary = palette::blurple;
    theme.on_primary = palette::white;
    theme.secondary = palette::input;
    theme.on_secondary = palette::header;
    theme.accent = palette::blurple;
    theme.outline = palette::selected;
    theme.focus = palette::white;
    theme.border = 0.0f;
    theme.radius = 8.0f;
    theme.radius_card = 12.0f;
    return theme;
}

ui::KeyboardLayout turkish_letters()
{
    ui::KeyboardLayout layout = ui::KeyboardLayout::letters();
    std::vector<ui::KeyboardKey> row;
    const char *pairs[][2] = {{"ç", "Ç"}, {"ğ", "Ğ"}, {"ı", "I"}, {"i", "İ"},
                              {"ö", "Ö"}, {"ş", "Ş"}, {"ü", "Ü"}};
    for (const auto &pair : pairs)
    {
        ui::KeyboardKey key;
        key.text = pair[0];
        key.shifted = pair[1];
        row.push_back(key);
    }
    layout.rows.insert(layout.rows.end() - 1, row);
    return layout;
}

// The modules of text as a QR code, row by row; size is 0 when it does not fit.
std::vector<bool> encode_qr(const std::string &text, int &size)
{
    std::vector<bool> modules;
    size = 0;
    uint8_t code[qrcodegen_BUFFER_LEN_MAX];
    uint8_t tmp[qrcodegen_BUFFER_LEN_MAX];
    if (text.empty() ||
        !qrcodegen_encodeText(text.c_str(), tmp, code, qrcodegen_Ecc_MEDIUM, qrcodegen_VERSION_MIN,
                              qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO, true))
        return modules;
    size = qrcodegen_getSize(code);
    modules.resize(static_cast<std::size_t>(size * size));
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x)
            modules[static_cast<std::size_t>(y * size + x)] = qrcodegen_getModule(code, x, y);
    return modules;
}

// A QR code in black on a white card.
void draw_qr(gfx::DrawList &list, const std::vector<bool> &modules, int size, const Rect &box,
             float inset)
{
    list.rounded_rect(box, 12, palette::white);
    const float module = (box.w - inset * 2) / static_cast<float>(size);
    for (int r = 0; r < size; ++r)
        for (int c = 0; c < size; ++c)
            if (modules[static_cast<std::size_t>(r * size + c)])
                list.rounded_rect({box.x + inset + c * module, box.y + inset + r * module,
                                   module + 0.5f, module + 0.5f},
                                  0, Color::rgb(0x000000));
}

// The in-app PSCord mark: a speech bubble with voice waves.
void draw_mark(gfx::DrawList &list, float cx, float cy, float scale, Color body, Color accent)
{
    const float s = scale;
    list.rounded_rect({cx - 20 * s, cy - 15 * s, 32 * s, 24 * s}, 9 * s, body);
    const float tail[] = {cx - 16 * s, cy + 6 * s, cx - 19 * s,
                          cy + 15 * s, cx - 8 * s, cy + 9 * s};
    list.polygon(tail, 3, body);
    for (float dx : {-12.0f, -4.0f, 4.0f})
        list.circle(cx + dx * s, cy - 3 * s, 2.6f * s, accent);
    list.arc(cx + 12 * s, cy - 3 * s, 7 * s, 2.6f * s, -0.75f, 1.5f, body);
    list.arc(cx + 12 * s, cy - 3 * s, 12 * s, 2.6f * s, -0.75f, 1.5f,
             body.with_alpha(body.a * 0.6f));
}

void draw_focus_ring(gfx::DrawList &list, const Rect &r, float radius)
{
    list.bordered_rect({r.x - 4, r.y - 4, r.w + 8, r.h + 8}, radius + 4, palette::clear, 3,
                       palette::white);
}

// A microphone, struck through in red when muted.
void draw_mic(gfx::DrawList &list, float cx, float cy, float s, Color color, bool muted)
{
    list.rounded_rect({cx - s * 0.2f, cy - s * 0.5f, s * 0.4f, s * 0.62f}, s * 0.2f, color);
    list.arc(cx, cy - s * 0.05f, s * 0.38f, s * 0.09f, 0.0f, 3.14159f, color);
    list.line(cx, cy + s * 0.33f, cx, cy + s * 0.5f, s * 0.09f, color);
    list.line(cx - s * 0.2f, cy + s * 0.5f, cx + s * 0.2f, cy + s * 0.5f, s * 0.09f, color);
    if (muted)
        list.line(cx - s * 0.45f, cy - s * 0.5f, cx + s * 0.45f, cy + s * 0.55f, s * 0.11f,
                  palette::red);
}

// The red "leave call" button's handset.
void draw_hang_up(gfx::DrawList &list, float cx, float cy, float s, Color color)
{
    list.rounded_rect({cx - s * 0.5f, cy - s * 0.14f, s, s * 0.24f}, s * 0.12f, color);
    list.rounded_rect({cx - s * 0.5f, cy - s * 0.04f, s * 0.24f, s * 0.24f}, s * 0.08f, color);
    list.rounded_rect({cx + s * 0.26f, cy - s * 0.04f, s * 0.24f, s * 0.24f}, s * 0.08f, color);
}

// Call tiles take a soft colour per person, as Discord does without video.
Color tile_color(const std::string &key)
{
    static const std::uint32_t kTiles[] = {0xc9d6ff, 0x5b8def, 0xff7aa2, 0xf6e3b4,
                                           0xffb067, 0x9be3c3, 0xc3a6ff};
    return Color::rgb(kTiles[std::hash<std::string>{}(key) % (sizeof(kTiles) / sizeof(kTiles[0]))]);
}

// A red count bubble, as on server icons and channels with mentions.
void draw_badge(gfx::DrawList &list, const ui::Fonts &fonts, float right, float cy, int count,
                Color ring)
{
    const std::string text = count > 99 ? "99+" : std::to_string(count);
    const float w = std::max(24.0f, fonts.semibold.measure(text, 18) + 14);
    if (ring.a > 0.0f)
        list.rounded_rect({right - w - 6, cy - 18, w + 12, 36}, 18, ring);
    list.rounded_rect({right - w, cy - 12, w, 24}, 12, palette::red);
    ui::text(list, fonts.semibold, text, right - w * 0.5f, cy + 6.5f, 18, palette::white,
             gfx::Align::center);
}

// ---- the screen -----------------------------------------------------------

class Discord final : public app::Concept
{
  public:
    explicit Discord(app::Context &context) : context_(context)
    {
        keys_.style.theme = keyboard_theme();
        keys_.style.bindings = ui::KeyboardBindings::standard();
        // R1 sends: Square already deletes, and navigating to Done on a stick
        // is slow for chat. The Done key shows the R1 glyph next to "Send".
        keys_.style.bindings.done = Action::page_next;
        keys_.style.done_label = T("Gönder");
        keys_.style.space_label = T("Boşluk");
        keys_.set_layouts({turkish_letters(), ui::KeyboardLayout::symbols()});
        keys_.set_bounds({360.0f, 580.0f, 1200.0f, 440.0f});
        keys_.on_text = [this](std::string_view utf8) { draft_.append(utf8); };
        keys_.on_backspace = [this] { pop_utf8(draft_); };
    }

    const app::ConceptInfo &info() const override
    {
        static const app::ConceptInfo kInfo{
            "discord",
            "PSCord",
            "Servers, channels and messages",
            "src/concepts/discord.cpp",
            audio::SoundSet::glass,
            palette::blurple,
            kTechniques,
        };
        return kInfo;
    }

    std::span<const app::TourStep> tour() const override
    {
        static const app::TourStep kSteps[] = {
            {1.5f, 0, Direction::none, "home"},
            {0.3f, action_bit(Action::page_next)},
            {0.3f, action_bit(Action::confirm)},
            {2.5f, 0, Direction::none, "chat"},
            {0.2f, 0, Direction::up},
            {0.2f, 0, Direction::up},
            {0.2f, 0, Direction::up},
            {0.2f, 0, Direction::up},
            {0.2f, 0, Direction::up},
            {0.2f, 0, Direction::up},
            {1.5f, 0, Direction::none, "scrolled"},
            {0.3f, action_bit(Action::menu)},
            {0.6f, 0, Direction::none, "settings"},
            {0.2f, 0, Direction::down},
            {0.2f, action_bit(Action::confirm)},
            {0.2f, 0, Direction::down},
            {0.2f, action_bit(Action::confirm)},
            {0.6f, 0, Direction::none, "settings-language"},
            {0.2f, action_bit(Action::back)},
            {0.2f, 0, Direction::down},
            {0.6f, 0, Direction::none, "settings-sound"},
            {0.2f, 0, Direction::down},
            {0.6f, 0, Direction::none, "settings-about"},
            {0.2f, 0, Direction::down},
            {0.2f, action_bit(Action::confirm)},
            {0.6f, 0, Direction::none, "logout"},
            {0.3f, action_bit(Action::back)},
            {0.3f, action_bit(Action::menu)},
            {0.3f, action_bit(Action::west)},
            {0.6f, 0, Direction::none, "compose"},
            {0.3f, action_bit(Action::back)},
            {0.3f, 0, Direction::left},
            {0.1f, 0, Direction::down},
            {0.1f, 0, Direction::down},
            {0.1f, 0, Direction::down},
            {0.1f, 0, Direction::down},
            {0.1f, 0, Direction::down},
            {0.1f, 0, Direction::down},
            {0.1f, 0, Direction::down},
            {0.1f, 0, Direction::down},
            {0.1f, 0, Direction::down},
            {0.1f, 0, Direction::down},
            {0.1f, 0, Direction::down},
            {0.1f, 0, Direction::down},
            {0.1f, 0, Direction::down},
            {0.1f, 0, Direction::down},
            {0.1f, 0, Direction::down},
            {0.1f, 0, Direction::down},
            {1.0f, 0, Direction::none, "voice"},
            {0.3f, action_bit(Action::confirm)},
            {1.0f, 0, Direction::none, "call"},
        };
        return kSteps;
    }

    void update(const InputFrame &input, float dt, app::Feedback &feedback) override
    {
        clock_ += dt;
        images_.update();
        if (dc::Client::instance().snapshot_if_newer(snap_.revision, snap_))
            refresh();

        update::Updater &updater = update::Updater::instance();
        if (updater.blocking())
        {
            if (updater.status().phase == update::Phase::failed &&
                input.is_pressed(Action::confirm))
            {
                updater.retry();
                feedback.play(audio::Cue::select);
            }
            return;
        }

        if (composing_)
        {
            update_compose(input, dt, feedback);
            return;
        }
        if (snap_.state == dc::State::connecting || snap_.state == dc::State::failed ||
            snap_.state == dc::State::starting)
        {
            if (input.is_pressed(Action::confirm))
            {
                dc::Client::instance().retry_connect();
                feedback.play(audio::Cue::select);
            }
            else if (input.is_pressed(Action::back))
            {
                dc::Client::instance().logout();
                feedback.play(audio::Cue::back);
            }
            return;
        }
        if (snap_.state != dc::State::ready)
            return;

        if (!snap_.invite.code.empty())
        {
            update_invite(input, feedback);
            return;
        }
        if (confirm_logout_)
        {
            update_logout(input, feedback);
            return;
        }
        if (settings_open_)
        {
            update_settings(input, feedback);
            return;
        }
        if (input.is_pressed(Action::menu))
        {
            settings_open_ = true;
            settings_section_ = 0;
            settings_content_ = false;
            feedback.play(audio::Cue::modal_open);
            return;
        }
        if (input.is_pressed(Action::north) && !snap_.voice_channel_id.empty())
        {
            dc::Client::instance().leave_voice();
            call_view_ = false;
            feedback.play(audio::Cue::back);
            return;
        }
        if (input.is_pressed(Action::back) && in_call_view())
        {
            call_view_ = false;
            feedback.play(audio::Cue::back);
            return;
        }
        if (input.is_pressed(Action::jump_next) && !snap_.voice_channel_id.empty())
        {
            dc::Client::instance().set_voice_muted(!snap_.voice_muted);
            feedback.play(audio::Cue::select);
            return;
        }
        // A forum channel takes posts, not messages: write inside a post instead.
        if (input.is_pressed(Action::west) && !snap_.channel_id.empty() && !in_post_list())
        {
            composing_ = true;
            keys_.set_layout(0);
            feedback.play(audio::Cue::open);
            return;
        }
        if (input.is_pressed(Action::page_next) || input.is_pressed(Action::page_prev))
        {
            select_guild(guild_ + (input.is_pressed(Action::page_next) ? 1 : -1));
            column_ = Column::channels;
            feedback.play(audio::Cue::tab);
            return;
        }
        navigate(input, feedback);
    }

    void draw(app::Frame &frame) const override
    {
        auto &list = frame.scene;
        frame.backdrop = ui::themes()[0].backdrop;
        frame.backdrop.time = clock_;
        if (snap_.state == dc::State::login)
            draw_login(list);
        else if (snap_.state == dc::State::ready)
            draw_app(list);
        else
            draw_loading(list);
        if (composing_)
            draw_compose(frame.overlay);
        if (settings_open_ && snap_.state == dc::State::ready)
            draw_settings(frame.overlay);
        if (confirm_logout_ && snap_.state == dc::State::ready)
            draw_logout(frame.overlay);
        if (!snap_.invite.code.empty() && snap_.state == dc::State::ready && !composing_)
            draw_invite(frame.overlay);
        if (update::Updater::instance().blocking())
            draw_update(frame.overlay);
    }

  private:
    // ---- input ------------------------------------------------------------

    const std::vector<dc::Channel> &channels() const
    {
        static const std::vector<dc::Channel> kNone;
        return guild_ < static_cast<int>(snap_.guilds.size())
                   ? snap_.guilds[static_cast<std::size_t>(guild_)].channels
                   : kNone;
    }

    int first_channel() const
    {
        const auto &list = channels();
        for (std::size_t i = 0; i < list.size(); ++i)
            if (list[i].type != 4)
                return static_cast<int>(i);
        return 0;
    }

    void select_guild(int index)
    {
        const int count = static_cast<int>(snap_.guilds.size());
        if (count == 0)
            return;
        guild_ = (index + count) % count;
        channel_ = first_channel();
    }

    void step_channel(int step)
    {
        const auto &list = channels();
        for (int i = channel_ + step; i >= 0 && i < static_cast<int>(list.size()); i += step)
        {
            if (list[static_cast<std::size_t>(i)].type != 4)
            {
                channel_ = i;
                return;
            }
        }
    }

    void navigate(const InputFrame &input, app::Feedback &feedback)
    {
        if (input.nav == Direction::left && column_ != Column::guilds)
        {
            column_ = column_ == Column::messages ? Column::channels : Column::guilds;
            feedback.play(audio::Cue::focus);
        }
        else if (input.nav == Direction::right && column_ != Column::messages)
        {
            column_ = column_ == Column::guilds ? Column::channels : Column::messages;
            feedback.play(audio::Cue::focus);
        }
        else if (input.nav == Direction::up || input.nav == Direction::down)
        {
            const int step = input.nav == Direction::up ? -1 : 1;
            if (column_ == Column::guilds)
                select_guild(std::clamp(guild_ + step, 0,
                                        std::max(0, static_cast<int>(snap_.guilds.size()) - 1)));
            else if (column_ == Column::channels)
                step_channel(step);
            else if (in_post_list())
                post_ =
                    std::clamp(post_ + step, 0, std::max(0, static_cast<int>(posts().size()) - 1));
            else
                scroll_ = std::clamp(scroll_ - step, 0,
                                     std::max(0, static_cast<int>(snap_.messages.size()) - 1));
            feedback.play(audio::Cue::focus);
        }

        if (input.is_pressed(Action::confirm))
        {
            const auto &list = channels();
            if (column_ == Column::guilds)
                column_ = Column::channels;
            else if (column_ == Column::messages && in_post_list() &&
                     post_ < static_cast<int>(posts().size()))
            {
                dc::Client::instance().open_post(snap_.channel_id,
                                                 posts()[static_cast<std::size_t>(post_)].id);
                scroll_ = 0;
            }
            else if (column_ == Column::channels && channel_ < static_cast<int>(list.size()) &&
                     list[static_cast<std::size_t>(channel_)].type != 4)
            {
                const dc::Channel &picked = list[static_cast<std::size_t>(channel_)];
                if (is_voice(picked))
                {
                    dc::Client::instance().join_voice(
                        snap_.guilds[static_cast<std::size_t>(guild_)].id, picked.id);
                    call_view_ = true;
                }
                else
                {
                    call_view_ = false;
                    dc::Client::instance().open_channel(picked.id);
                    column_ = Column::messages;
                    scroll_ = 0;
                }
            }
            feedback.play(audio::Cue::select);
        }
        else if (input.is_pressed(Action::back) && in_post() && column_ == Column::messages)
        {
            // Back out of a post to the list of posts, not to the channels.
            dc::Client::instance().open_channel(snap_.forum_id);
            feedback.play(audio::Cue::back);
        }
        else if (input.is_pressed(Action::back) && column_ != Column::guilds)
        {
            column_ = column_ == Column::messages ? Column::channels : Column::guilds;
            feedback.play(audio::Cue::back);
        }
    }

    // Signing out needs a second, deliberate choice: focus starts on T("İptal").
    void update_logout(const InputFrame &input, app::Feedback &feedback)
    {
        if (input.nav == Direction::left || input.nav == Direction::right)
        {
            logout_focus_ = input.nav == Direction::right ? 1 : 0;
            feedback.play(audio::Cue::focus);
        }
        if (input.is_pressed(Action::back) || input.is_pressed(Action::menu) ||
            (input.is_pressed(Action::confirm) && logout_focus_ == 0))
        {
            confirm_logout_ = false;
            feedback.play(audio::Cue::back);
        }
        else if (input.is_pressed(Action::confirm))
        {
            confirm_logout_ = false;
            settings_open_ = false;
            dc::Client::instance().logout();
            feedback.play(audio::Cue::select);
        }
    }

    void update_invite(const InputFrame &input, app::Feedback &feedback)
    {
        dc::Client &client = dc::Client::instance();
        switch (snap_.invite.join)
        {
        case dc::Invite::Join::joining:
            return;
        case dc::Invite::Join::idle:
            if (input.nav == Direction::up || input.nav == Direction::down)
            {
                invite_focus_ = 1 - invite_focus_;
                feedback.play(audio::Cue::focus);
            }
            else if (input.is_pressed(Action::back) ||
                     (input.is_pressed(Action::confirm) && invite_focus_ == 1))
            {
                client.dismiss_invite();
                feedback.play(audio::Cue::back);
            }
            else if (input.is_pressed(Action::confirm))
            {
                client.join_invite();
                feedback.play(audio::Cue::select);
            }
            return;
        default:
            if (input.is_pressed(Action::confirm) || input.is_pressed(Action::back))
            {
                client.dismiss_invite();
                feedback.play(audio::Cue::back);
            }
            return;
        }
    }

    void draw_invite(gfx::DrawList &over) const
    {
        const auto &fonts = context_.fonts;
        const dc::Invite &invite = snap_.invite;
        const bool failed = invite.join == dc::Invite::Join::failed && invite_qr_size_ > 0;
        over.rounded_rect({0, 0, 1920, 1080}, 0, Color::rgb(0x000000, 0.7f));
        const Rect card{660, failed ? 210.0f : 310.0f, 600, failed ? 660.0f : 460.0f};
        over.shadow(card, 12, 48, Color::rgb(0x000000, 0.4f));
        over.rounded_rect(card, 12, palette::chat);

        const float cx = card.x + card.w * 0.5f;
        draw_avatar(over, invite.icon_url, cx, card.y + 88, 48, invite.code, invite.guild_name);
        ui::text(over, fonts.regular, T("PSCord topluluğuna davet edildin"), cx, card.y + 176, 20,
                 palette::muted, gfx::Align::center);
        const float name_w = std::min(520.0f, fonts.semibold.measure(invite.guild_name, 30));
        label(over, fonts.semibold, invite.guild_name, cx - name_w * 0.5f, card.y + 218, 30,
              palette::header, 520);
        const std::string online = T("%s Çevrimiçi", std::to_string(invite.online));
        const std::string members = T("%s Üye", std::to_string(invite.members));
        const float online_w = fonts.regular.measure(online, 20);
        const float members_w = fonts.regular.measure(members, 20);
        float x = cx - (18 + online_w + 32 + 18 + members_w) * 0.5f;
        over.circle(x + 6, card.y + 255, 6, palette::green);
        ui::text(over, fonts.regular, online, x + 18, card.y + 262, 20, palette::normal);
        x += 18 + online_w + 32;
        over.circle(x + 6, card.y + 255, 6, palette::muted);
        ui::text(over, fonts.regular, members, x + 18, card.y + 262, 20, palette::normal);

        if (failed)
        {
            ui::text(over, fonts.regular, T("Buradan katılamadık. Telefonunla okutup katıl:"), cx,
                     card.y + 314, 20, palette::normal, gfx::Align::center);
            draw_qr(over, invite_qr_, invite_qr_size_, {cx - 120, card.y + 340, 240, 240}, 16);
            ui::text(over, fonts.semibold, "discord.gg/" + invite.code, cx, card.y + 616, 22,
                     palette::header, gfx::Align::center);
            return;
        }

        const Rect join{card.x + 40, card.y + card.h - 150, card.w - 80, 56};
        const Rect later{join.x, join.y + 72, join.w, 44};
        const bool busy = invite.join == dc::Invite::Join::joining;
        const bool joined = invite.join == dc::Invite::Join::joined;
        over.rounded_rect(join, 6, joined ? palette::green : palette::blurple);
        ui::text(over, fonts.semibold,
                 joined ? T("Katıldın!")
                 : busy ? T("Katılınıyor...")
                        : T("Sunucuya Katıl"),
                 join.x + join.w * 0.5f, join.y + 36, 22, palette::white, gfx::Align::center);
        if (!joined)
            ui::text(over, fonts.regular, T("Daha Sonra"), later.x + later.w * 0.5f, later.y + 29,
                     20, invite_focus_ == 1 ? palette::header : palette::muted, gfx::Align::center);
        if (!busy)
            draw_focus_ring(over, invite_focus_ == 0 || joined ? join : later, 6);
    }

    void draw_logout(gfx::DrawList &over) const
    {
        const auto &fonts = context_.fonts;
        over.rounded_rect({0, 0, 1920, 1080}, 0, Color::rgb(0x000000, 0.7f));
        const Rect card{960 - 330, 540 - 135, 660, 270};
        over.shadow(card, 12, 48, Color::rgb(0x000000, 0.4f));
        over.rounded_rect(card, 12, palette::chat);
        ui::text(over, fonts.semibold, T("Çıkış Yap"), card.x + 36, card.y + 66, 30,
                 palette::header);
        ui::text(over, fonts.regular, T("Çıkış yapmak istediğine emin misin?"), card.x + 36,
                 card.y + 116, 24, palette::normal);
        over.rounded_rect({card.x, card.y + card.h - 96, card.w, 96}, 12, palette::sidebar);
        over.rounded_rect({card.x, card.y + card.h - 96, card.w, 24}, 0, palette::sidebar);
        const Rect quit{card.x + card.w - 36 - 180, card.y + card.h - 72, 180, 54};
        const Rect cancel{quit.x - 24 - 150, quit.y, 150, 54};
        over.rounded_rect(cancel, 6, logout_focus_ == 0 ? palette::selected : palette::sidebar);
        ui::text(over, fonts.semibold, T("İptal"), cancel.x + cancel.w * 0.5f, cancel.y + 35, 22,
                 palette::header, gfx::Align::center);
        over.rounded_rect(quit, 6, palette::red);
        ui::text(over, fonts.semibold, T("Çıkış Yap"), quit.x + quit.w * 0.5f, quit.y + 35, 22,
                 palette::white, gfx::Align::center);
        draw_focus_ring(over, logout_focus_ == 0 ? cancel : quit, 6);
    }

    void update_compose(const InputFrame &input, float dt, app::Feedback &feedback)
    {
        keys_.set_length(static_cast<int>(draft_.size()));
        const ui::Event event = keys_.handle(input, feedback);
        if (event == ui::Event::activated)
        {
            if (!draft_.empty())
                dc::Client::instance().send_message(draft_);
            draft_.clear();
            composing_ = false;
            scroll_ = 0;
        }
        else if (event == ui::Event::cancelled || input.is_pressed(Action::back))
        {
            composing_ = false;
        }
        keys_.update(dt);
    }

    void refresh()
    {
        if (snap_.qr_url != qr_url_)
        {
            qr_url_ = snap_.qr_url;
            qr_ = encode_qr(qr_url_, qr_size_);
        }
        if (snap_.invite.code != invite_code_)
        {
            invite_code_ = snap_.invite.code;
            invite_qr_ = encode_qr(invite_code_.empty() ? "" : "https://discord.gg/" + invite_code_,
                                   invite_qr_size_);
            invite_focus_ = 0;
        }
        if (snap_.channel_id != rich_channel_)
        {
            rich_channel_ = snap_.channel_id;
            rich_.clear();
        }
        if (guild_ >= static_cast<int>(snap_.guilds.size()))
            guild_ = 0;
        post_ = std::clamp(post_, 0, std::max(0, static_cast<int>(posts().size()) - 1));
        const int count = static_cast<int>(channels().size());
        if (channel_ >= count ||
            (count > 0 && channels()[static_cast<std::size_t>(channel_)].type == 4))
            channel_ = first_channel();
        scroll_ = std::min(scroll_, std::max(0, static_cast<int>(snap_.messages.size()) - 1));
    }

    // A forum post is a channel of its own, so while one is open the forum it
    // came from is what the sidebar and the header follow.
    const dc::Channel *open_channel() const
    {
        const std::string &id = snap_.forum_id.empty() ? snap_.channel_id : snap_.forum_id;
        for (const dc::Guild &g : snap_.guilds)
            for (const dc::Channel &c : g.channels)
                if (c.id == id)
                    return &c;
        return nullptr;
    }

    bool in_post() const
    {
        return !snap_.forum_id.empty();
    }

    // True while the chat column lists the posts of a forum instead of messages.
    bool in_post_list() const
    {
        const dc::Channel *open = open_channel();
        return open && is_forum(*open) && !in_post();
    }

    const std::vector<dc::ForumPost> &posts() const
    {
        static const std::vector<dc::ForumPost> kNone;
        const dc::Channel *open = open_channel();
        return open ? open->posts : kNone;
    }

    std::string open_post_name() const
    {
        for (const dc::ForumPost &p : posts())
            if (p.id == snap_.channel_id)
                return p.name;
        return {};
    }

    const dc::Guild *open_guild() const
    {
        for (const dc::Guild &g : snap_.guilds)
            if (!g.id.empty() && g.id == snap_.guild_id)
                return &g;
        return nullptr;
    }

    bool show_members() const
    {
        const dc::Channel *open = open_channel();
        return open && !is_dm(*open) && !in_call_view();
    }

    const dc::Channel *call_channel() const
    {
        for (const dc::Guild &g : snap_.guilds)
            for (const dc::Channel &c : g.channels)
                if (!snap_.voice_channel_id.empty() && c.id == snap_.voice_channel_id)
                    return &c;
        return nullptr;
    }

    bool in_call_view() const
    {
        return call_view_ && call_channel() != nullptr;
    }

    // The call screen: a tile per person, the microphone and leave buttons below.
    void draw_call(gfx::DrawList &list) const
    {
        const auto &fonts = context_.fonts;
        const dc::Channel &call = *call_channel();
        const voice::Status status = voice::bg::live(voice::Engine::instance().status());
        const float x0 = layout::chat_x;
        const float w = 1920 - x0;
        list.rounded_rect({x0, 0, w, 1080}, 0, Color::rgb(0x000000));

        discord::draw_channel_icon(list, call.type, false, x0 + 24, 36, 36, palette::faint);
        label(list, fonts.semibold, call.name, x0 + 70, 45, 24, palette::header, 700);
        const ui::Hint hints[] = {
            {ui::Button::r2, snap_.voice_muted ? T("Sesi Aç") : T("Sessize Al")},
            {ui::Button::triangle, T("Ayrıl")},
            {ui::Button::circle, T("Geri")},
        };
        ui::HintLayout hint_layout;
        hint_layout.size = 30;
        hint_layout.text_size = 20;
        hint_layout.cy = 36;
        hint_layout.item_gap = 28;
        ui::GlyphStyle style = ui::GlyphStyle::dark();
        style.label = palette::muted;
        ui::draw_hints(list, fonts, style, hints, 3, 1920 - 24, true, hint_layout);

        const std::vector<dc::VoiceUser> &people = call.voice_users;
        const int n = std::max(1, static_cast<int>(people.size()));
        const int cols = n == 1 ? 1 : n <= 4 ? 2 : n <= 9 ? 3 : 4;
        const int rows = (n + cols - 1) / cols;
        const float gap = 16;
        const float area_x = x0 + 40;
        const float area_y = layout::header_h + 32;
        const float area_w = w - 80;
        const float area_h = 1080 - area_y - 180;
        float tile_w = (area_w - gap * static_cast<float>(cols - 1)) / static_cast<float>(cols);
        float tile_h = tile_w * 9.0f / 16.0f;
        if (tile_h * static_cast<float>(rows) + gap * static_cast<float>(rows - 1) > area_h)
        {
            tile_h = (area_h - gap * static_cast<float>(rows - 1)) / static_cast<float>(rows);
            tile_w = tile_h * 16.0f / 9.0f;
        }
        const float grid_h = tile_h * static_cast<float>(rows) + gap * static_cast<float>(rows - 1);
        const float top = area_y + (area_h - grid_h) * 0.5f;
        for (int i = 0; i < static_cast<int>(people.size()); ++i)
        {
            const dc::VoiceUser &u = people[static_cast<std::size_t>(i)];
            const int row = i / cols;
            const int in_row = std::min(cols, static_cast<int>(people.size()) - row * cols);
            const float row_w =
                tile_w * static_cast<float>(in_row) + gap * static_cast<float>(in_row - 1);
            const Rect tile{area_x + (area_w - row_w) * 0.5f +
                                static_cast<float>(i % cols) * (tile_w + gap),
                            top + static_cast<float>(row) * (tile_h + gap), tile_w, tile_h};
            const bool talking = status.speaking.count(u.id) > 0;
            const bool me = u.id == snap_.me_id;
            const bool muted = me ? snap_.voice_muted : u.muted;
            const std::string name = u.name.empty() ? std::string("…") : u.name;
            list.rounded_rect(tile, 12, tile_color(u.id));
            if (talking)
                list.bordered_rect(tile, 12, palette::clear, 4, palette::green);
            const float radius = std::min(80.0f, tile_h * 0.2f);
            draw_avatar(list, u.avatar_url, tile.x + tile.w * 0.5f, tile.y + tile.h * 0.5f, radius,
                        u.id, name);

            const float pill_w = std::min(tile.w - 24, fonts.semibold.measure(name, 20) + 24);
            list.rounded_rect({tile.x + 12, tile.y + tile.h - 48, pill_w, 36}, 8,
                              Color::rgb(0x000000, 0.45f));
            label(list, fonts.semibold, name, tile.x + 24, tile.y + tile.h - 23, 20, palette::white,
                  pill_w - 24);
            if (muted || u.deafened)
            {
                const float cx = tile.x + tile.w - 34;
                const float cy = tile.y + tile.h - 30;
                list.circle(cx, cy, 20, Color::rgb(0x000000, 0.45f));
                draw_mic(list, cx, cy, 22, palette::white, true);
            }
        }

        const float cx = x0 + w * 0.5f;
        const float cy = 1080 - 84;
        if (status.phase != voice::Phase::connected || !status.mic_error.empty())
        {
            const std::string note =
                status.phase != voice::Phase::connected ? status.text : status.mic_error;
            ui::text(list, fonts.regular, note, cx, cy - 64, 20, palette::muted,
                     gfx::Align::center);
        }
        list.circle(cx - 56, cy, 36, snap_.voice_muted ? palette::white : Color::rgb(0x2b2d31));
        draw_mic(list, cx - 56, cy, 30, snap_.voice_muted ? Color::rgb(0x2b2d31) : palette::white,
                 snap_.voice_muted);
        list.circle(cx + 56, cy, 36, palette::red);
        draw_hang_up(list, cx + 56, cy + 2, 34, palette::white);
        const ui::GlyphStyle glyphs = ui::GlyphStyle::dark();
        ui::draw_button(list, fonts, glyphs, ui::Button::r2,
                        cx - 56 - ui::button_width(ui::Button::r2, 24) * 0.5f, cy + 58, 24);
        ui::draw_button(list, fonts, glyphs, ui::Button::triangle,
                        cx + 56 - ui::button_width(ui::Button::triangle, 24) * 0.5f, cy + 58, 24);
    }

    float chat_right() const
    {
        return show_members() ? 1920 - layout::members_w : 1920;
    }

    // ---- rich text cache ----------------------------------------------------

    const RichText &rich(const std::string &key, const std::string &markdown, float width,
                         float base, Color color, const dc::Message *message,
                         bool edited = false) const
    {
        const std::string full = key + "|" + std::to_string(std::hash<std::string>{}(markdown)) +
                                 "|" + std::to_string(static_cast<int>(width)) +
                                 (edited ? "e" : "");
        auto it = rich_.find(full);
        if (it != rich_.end())
            return it->second;
        dc::md::Names names;
        names.user = [&](const std::string &id) -> std::string
        {
            if (message)
                for (const dc::Mention &m : message->mentions)
                    if (m.id == id)
                        return m.name;
            for (const dc::Member &m : snap_.members)
                if (m.id == id)
                    return m.label;
            return {};
        };
        names.channel = [&](const std::string &id) -> std::string
        {
            for (const dc::Guild &g : snap_.guilds)
                for (const dc::Channel &c : g.channels)
                    if (c.id == id)
                        return c.name;
            return {};
        };
        names.role = [&](const std::string &id) -> std::string
        {
            if (const dc::Guild *g = open_guild())
                for (const dc::Role &r : g->roles)
                    if (r.id == id)
                        return r.name;
            return {};
        };
        std::vector<dc::md::Block> blocks = dc::md::parse(markdown, names);
        if (edited)
        {
            if (blocks.empty())
                blocks.emplace_back();
            blocks.back().spans.push_back({T(" (düzenlendi)"), discord::kEdited, {}, 9999});
        }
        return rich_.emplace(full, discord::layout_rich(blocks, context_.fonts, width, base, color))
            .first->second;
    }

    // ---- small pieces that need the picture cache ---------------------------

    void draw_avatar(gfx::DrawList &list, const std::string &url, float cx, float cy, float radius,
                     const std::string &key, const std::string &name) const
    {
        const int px = radius > 20 ? 128 : 64;
        if (const std::uint32_t tex = images_.get(url, px, px, clock_))
        {
            list.image(tex, {cx - radius, cy - radius, radius * 2, radius * 2}, gfx::kFullUv,
                       palette::white, radius);
            return;
        }
        list.circle(cx, cy, radius, avatar_color(key));
        const float size = radius * 0.9f;
        ui::text(list, context_.fonts.semibold, first_letter(name), cx, cy + size * 0.36f, size,
                 palette::white, gfx::Align::center);
    }

    float label(gfx::DrawList &list, const ui::FontRef &font, const std::string &text, float x,
                float baseline, float size, Color color, float max_w) const
    {
        return discord::draw_label(list, context_.fonts, font, text, x, baseline, size, color,
                                   max_w, images_, clock_);
    }

    // A picture from a message: rounded, grey until it arrives, a play button on videos.
    void draw_media(gfx::DrawList &list, const dc::MediaRef &media, float x, float y) const
    {
        const Rect r{x, y, static_cast<float>(media.width), static_cast<float>(media.height)};
        const std::uint32_t tex = images_.get(media.url, media.width, media.height, clock_);
        if (tex)
            list.image(tex, r, gfx::kFullUv, palette::white, 12);
        else
            list.rounded_rect(r, 12, palette::code_bg);
        if (media.video)
        {
            const float cx = r.x + r.w * 0.5f;
            const float cy = r.y + r.h * 0.5f;
            list.circle(cx, cy, 36, Color::rgb(0x000000, 0.6f));
            list.triangle({cx - 12, cy - 15, 30, 30}, palette::white, 0.0f, 1.5708f);
        }
    }

    void draw_bot_tag(gfx::DrawList &list, float x, float baseline) const
    {
        const auto &fonts = context_.fonts;
        const float w = fonts.semibold.measure("UYG", 15) + 12;
        list.rounded_rect({x, baseline - 18, w, 22}, 5, palette::blurple);
        ui::text(list, fonts.semibold, "UYG", x + 6, baseline - 1, 15, palette::white);
    }

    // ---- sign-in ------------------------------------------------------------

    void draw_login(gfx::DrawList &list) const
    {
        const auto &fonts = context_.fonts;
        list.gradient_rect({0, 0, 1920, 1080}, 0, palette::blurple, palette::blurple_deep);
        draw_mark(list, 96, 72, 1.2f, palette::white, palette::blurple);
        ui::text(list, fonts.semibold, "PSCord", 136, 84, 32, palette::white);

        const Rect card{372, 252, 1176, 576};
        list.shadow(card, 12, 48, Color::rgb(0x000000, 0.35f));
        list.rounded_rect(card, 12, palette::chat);

        const float left = card.x + 48;
        const float left_w = 640;
        const float mid = left + left_w * 0.5f;
        ui::text(list, fonts.semibold, T("Tekrar hoş geldin!"), mid, card.y + 96, 36,
                 palette::header, gfx::Align::center);
        ui::text(list, fonts.regular, T("Seni tekrar gördüğümüze çok sevindik!"), mid, card.y + 140,
                 24, palette::interactive, gfx::Align::center);

        float y = card.y + 212;
        ui::text(list, fonts.semibold, T("BİLGİSAYARDAN GİRİŞ YAP"), left, y, 18,
                 palette::interactive);
        const Rect field{left, y + 16, left_w, 72};
        list.rounded_rect(field, 6, palette::rail);
        if (!snap_.web_url.empty())
        {
            ui::text(list, fonts.semibold, snap_.web_url, field.x + 24, field.y + 48, 32,
                     palette::header);
            y = field.y + field.h + 44;
            ui::text(list, fonts.regular,
                     T("Aynı ağdaki bir bilgisayarda bu adresi tarayıcıya yaz,"), left, y, 22,
                     palette::interactive);
            ui::text(list, fonts.regular, T("açılan sayfada e-posta ve şifrenle giriş yap."), left,
                     y + 32, 22, palette::interactive);
        }
        else
        {
            const std::string why =
                snap_.web_error.empty() ? T("Hazırlanıyor...") : snap_.web_error;
            label(list, fonts.regular, why, field.x + 24, field.y + 46, 24,
                  snap_.web_error.empty() ? palette::muted : palette::red, left_w - 48);
            y = field.y + field.h + 44;
        }

        y += 72;
        if (!snap_.pending_user.empty())
        {
            list.rounded_rect({left, y - 40, left_w, 64}, 6, palette::blurple);
            ui::text(list, fonts.semibold, T("%s olarak giriş yapılıyor", snap_.pending_user), mid,
                     y + 2, 24, palette::white, gfx::Align::center);
        }
        else if (!snap_.status.empty())
        {
            label(list, fonts.regular, snap_.status, left, y, 20, palette::muted, left_w);
        }

        const float right_mid = card.x + card.w - 48 - 204;
        const float side = 296;
        const Rect box{right_mid - side * 0.5f, card.y + 72, side, side};
        list.rounded_rect(box, 12, palette::white);
        if (qr_size_ > 0)
        {
            draw_qr(list, qr_, qr_size_, box, 20);
            list.rounded_rect({box.x + side * 0.5f - 36, box.y + side * 0.5f - 36, 72, 72}, 16,
                              palette::white);
            list.rounded_rect({box.x + side * 0.5f - 30, box.y + side * 0.5f - 30, 60, 60}, 14,
                              palette::blurple);
            draw_mark(list, box.x + side * 0.5f, box.y + side * 0.5f + 2, 0.9f, palette::white,
                      palette::blurple);
        }
        else
        {
            ui::text(list, fonts.regular, T("Hazırlanıyor..."), right_mid, box.y + side * 0.5f + 8,
                     22, palette::muted, gfx::Align::center);
        }
        ui::text(list, fonts.semibold, T("QR Kodu ile Giriş Yap"), right_mid, box.y + side + 64, 30,
                 palette::header, gfx::Align::center);
        ui::text(list, fonts.regular, T("Anında giriş yapmak için Discord"), right_mid,
                 box.y + side + 108, 22, palette::interactive, gfx::Align::center);
        ui::text(list, fonts.regular, T("mobil uygulamasıyla tara."), right_mid, box.y + side + 140,
                 22, palette::interactive, gfx::Align::center);
    }

    void draw_loading(gfx::DrawList &list) const
    {
        const auto &fonts = context_.fonts;
        list.rounded_rect({0, 0, 1920, 1080}, 0, palette::chat);
        const float bob = std::sin(clock_ * 3.0f) * 8.0f;
        draw_mark(list, 960, 470 + bob, 3.0f, palette::blurple, palette::chat);
        const std::string status =
            snap_.status.empty() ? std::string(T("Bağlanılıyor...")) : snap_.status;
        ui::text(list, fonts.semibold, T("BİLİYOR MUYDUN"), 960, 600, 18, palette::muted,
                 gfx::Align::center);
        ui::text(list, fonts.regular, status, 960, 644, 26, palette::normal, gfx::Align::center);
        const ui::Hint hints[] = {
            {ui::Button::cross, T("Yeniden Dene")},
            {ui::Button::circle, T("Çıkış Yap")},
        };
        ui::HintLayout hint_layout;
        hint_layout.size = 30;
        hint_layout.text_size = 20;
        hint_layout.cy = 1010;
        hint_layout.item_gap = 28;
        ui::GlyphStyle style = ui::GlyphStyle::dark();
        style.label = palette::muted;
        const float hints_w = ui::measure_hints(fonts, hints, 2, hint_layout);
        ui::draw_hints(list, fonts, style, hints, 2, 960 - hints_w * 0.5f, false, hint_layout);
    }

    // Covers everything while an update is checked for, downloaded and installed.
    void draw_update(gfx::DrawList &over) const
    {
        const auto &fonts = context_.fonts;
        const update::Status status = update::Updater::instance().status();
        over.rounded_rect({0, 0, 1920, 1080}, 0, palette::chat);
        const float bob = std::sin(clock_ * 3.0f) * 8.0f;
        draw_mark(over, 960, 400 + bob, 3.0f, palette::blurple, palette::chat);

        std::string title = T("Güncellemeler denetleniyor...");
        std::string detail;
        float progress = -1;
        switch (status.phase)
        {
        case update::Phase::downloading:
            title = T("PSCord %s indiriliyor", status.version);
            if (status.total > 0)
            {
                progress = std::min(1.0f, static_cast<float>(status.received) /
                                              static_cast<float>(status.total));
                char text[64];
                std::snprintf(text, sizeof(text), "%.1f / %.1f MB", status.received / 1048576.0,
                              status.total / 1048576.0);
                detail = text;
            }
            break;
        case update::Phase::installing:
            title = T("PSCord %s kuruluyor", status.version);
            progress = 1;
            break;
        case update::Phase::restarting:
            title = T("Güncelleme tamamlandı, yeniden başlatılıyor");
            progress = 1;
            break;
        case update::Phase::failed:
            title = T(status.error.c_str());
            detail = T("Tekrar denemek için X'e bas");
            break;
        default:
            break;
        }
        ui::text(over, fonts.semibold, title, 960, 540, 32, palette::header, gfx::Align::center);
        if (progress >= 0)
        {
            const Rect bar{960 - 300, 584, 600, 10};
            over.rounded_rect(bar, 5, palette::sidebar);
            over.rounded_rect({bar.x, bar.y, bar.w * progress, bar.h}, 5, palette::blurple);
        }
        if (!detail.empty())
            ui::text(over, fonts.regular, detail, 960, 640, 24, palette::muted, gfx::Align::center);
        if (!status.notes.empty() && status.phase != update::Phase::failed)
            ui::text(over, fonts.regular, status.notes, 960, 700, 22, palette::normal,
                     gfx::Align::center);
    }

    // ---- settings -------------------------------------------------------------

    enum class Section
    {
        account,
        language,
        sound,
        about,
        logout,
        count
    };

    static const char *section_name(Section section)
    {
        constexpr const char *kNames[] = {"Hesabım", "Dil", "Ses ve Kontroller", "Hakkında",
                                          "Çıkış Yap"};
        return T(kNames[static_cast<int>(section)]);
    }

    static int section_rows(Section section)
    {
        if (section == Section::language)
            return i18n::kLangCount;
        if (section == Section::sound)
            return 4;
        if (section == Section::about)
            return 2;
        return 0;
    }

    void change_setting(int row, int step, app::Feedback &feedback)
    {
        Settings &settings = context_.settings;
        const Section section = static_cast<Section>(settings_section_);
        if (section == Section::language)
        {
            if (step != 0)
                return;
            settings.language = row;
            i18n::set_language(static_cast<i18n::Lang>(row));
            rich_.clear();
        }
        else if (row == 0)
        {
            const int volume =
                step == 0 ? (settings.ui_volume > 0 ? 0 : 7) : settings.ui_volume + step;
            settings.ui_volume = std::clamp(volume, 0, 10);
        }
        else
        {
            bool &flag = row == 1   ? settings.haptics
                         : row == 2 ? settings.light_bar
                                    : settings.swap_confirm;
            flag = !flag;
        }
        context_.settings_changed = true;
        feedback.play(audio::Cue::toggle);
    }

    void update_settings(const InputFrame &input, app::Feedback &feedback)
    {
        const Section section = static_cast<Section>(settings_section_);
        const int rows = section_rows(section);
        if (!settings_content_)
        {
            const int count = static_cast<int>(Section::count);
            if (input.nav == Direction::up || input.nav == Direction::down)
            {
                settings_section_ =
                    (settings_section_ + (input.nav == Direction::down ? 1 : count - 1)) % count;
                feedback.play(audio::Cue::focus);
            }
            else if (input.is_pressed(Action::back) || input.is_pressed(Action::menu))
            {
                settings_open_ = false;
                feedback.play(audio::Cue::modal_close);
            }
            else if (input.is_pressed(Action::confirm) || input.nav == Direction::right)
            {
                if (section == Section::logout && input.is_pressed(Action::confirm))
                {
                    confirm_logout_ = true;
                    logout_focus_ = 0;
                    feedback.play(audio::Cue::open);
                }
                else if (rows > 0)
                {
                    settings_content_ = true;
                    settings_row_ = section == Section::language ? context_.settings.language : 0;
                    feedback.play(audio::Cue::select);
                }
            }
            return;
        }

        if (input.nav == Direction::up || input.nav == Direction::down)
        {
            settings_row_ = (settings_row_ + (input.nav == Direction::down ? 1 : rows - 1)) % rows;
            feedback.play(audio::Cue::focus);
        }
        else if (input.is_pressed(Action::menu))
        {
            settings_open_ = false;
            feedback.play(audio::Cue::modal_close);
        }
        else if (input.is_pressed(Action::back) ||
                 (input.nav == Direction::left &&
                  (section != Section::sound || settings_row_ != 0)))
        {
            settings_content_ = false;
            feedback.play(audio::Cue::back);
        }
        else if (input.is_pressed(Action::confirm))
        {
            if (section == Section::about)
            {
                if (settings_row_ == 0)
                {
                    dc::Client::instance().show_community_invite();
                    feedback.play(audio::Cue::select);
                }
                return;
            }
            change_setting(settings_row_, 0, feedback);
        }
        else if (section == Section::sound &&
                 (input.nav == Direction::left || input.nav == Direction::right))
        {
            change_setting(settings_row_,
                           settings_row_ == 0 ? (input.nav == Direction::right ? 1 : -1) : 0,
                           feedback);
        }
    }

    void draw_switch(gfx::DrawList &over, float right, float cy, bool on) const
    {
        const Rect track{right - 52, cy - 15, 52, 30};
        over.rounded_rect(track, 15, on ? palette::green : Color::rgb(0x72767d));
        over.circle(on ? track.x + track.w - 15 : track.x + 15, cy, 11, palette::white);
    }

    void draw_settings(gfx::DrawList &over) const
    {
        const auto &fonts = context_.fonts;
        const Settings &settings = context_.settings;
        const Section current = static_cast<Section>(settings_section_);
        over.rounded_rect({0, 0, 1920, 1080}, 0, palette::chat);
        over.rounded_rect({0, 0, 620, 1080}, 0, palette::sidebar);

        // The section list, right-aligned in the sidebar like the desktop client.
        const float nav_x = 380;
        const float nav_w = 220;
        float y = 120;
        Rect focused{};
        const auto group = [&](const char *title)
        {
            ui::text(over, fonts.semibold, T(title), nav_x + 12, y, 15, palette::muted);
            y += 16;
        };
        const auto item = [&](Section section)
        {
            const Rect row{nav_x, y, nav_w, 44};
            const bool selected = section == current;
            if (selected)
                over.rounded_rect(row, 6, palette::selected);
            ui::text(over, fonts.semibold, section_name(section), row.x + 12, row.y + 29, 20,
                     section == Section::logout ? palette::red
                     : selected                 ? palette::header
                                                : palette::muted);
            if (selected)
                focused = row;
            y += 50;
        };
        group("KULLANICI AYARLARI");
        item(Section::account);
        y += 24;
        group("UYGULAMA AYARLARI");
        item(Section::language);
        item(Section::sound);
        item(Section::about);
        over.line(nav_x + 10, y + 4, nav_x + nav_w - 10, y + 4, 2, palette::divider);
        y += 20;
        item(Section::logout);
        if (!settings_content_)
            draw_focus_ring(over, focused, 6);

        const ui::Hint hints[] = {
            {ui::Button::cross, T("Seç")},
            {ui::Button::circle, T("Geri")},
            {ui::Button::options, T("Kapat")},
        };
        ui::HintLayout hint_layout;
        hint_layout.size = 30;
        hint_layout.text_size = 20;
        hint_layout.cy = 52;
        hint_layout.item_gap = 28;
        ui::GlyphStyle style = ui::GlyphStyle::dark();
        style.label = palette::muted;
        ui::draw_hints(over, fonts, style, hints, 3, 1920 - 48, true, hint_layout);

        const float x = 680;
        const float w = 760;
        ui::text(over, fonts.semibold, section_name(current), x, 150, 30, palette::header);
        y = 200;
        switch (current)
        {
        case Section::account:
        {
            const Rect card{x, y, w, 300};
            over.rounded_rect(card, 12, palette::rail);
            over.rounded_rect({card.x, card.y, card.w, 110}, 12, palette::blurple);
            over.rounded_rect({card.x, card.y + 98, card.w, 12}, 0, palette::rail);
            over.circle(card.x + 90, card.y + 120, 62, palette::rail);
            draw_avatar(over, snap_.me_avatar, card.x + 90, card.y + 120, 54, snap_.me_id,
                        snap_.me);
            label(over, fonts.semibold, snap_.me, card.x + 170, card.y + 158, 28, palette::header,
                  w - 200);
            const Rect field{card.x + 24, card.y + 196, card.w - 48, 80};
            over.rounded_rect(field, 8, palette::sidebar);
            ui::text(over, fonts.semibold, T("KULLANICI ADI"), field.x + 20, field.y + 30, 15,
                     palette::muted);
            label(over, fonts.regular, snap_.me, field.x + 20, field.y + 62, 22, palette::header,
                  field.w - 40);
            break;
        }
        case Section::language:
        {
            ui::text(over, fonts.regular, T("Uygulamanın dilini seç."), x, y + 10, 20,
                     palette::muted);
            y += 40;
            for (int i = 0; i < i18n::kLangCount; ++i)
            {
                const Rect row{x, y, w, 64};
                const bool chosen = settings.language == i;
                over.rounded_rect(row, 8, chosen ? palette::selected : palette::sidebar);
                over.circle(row.x + 36, row.y + 32, 12, chosen ? palette::white : palette::muted);
                over.circle(row.x + 36, row.y + 32, chosen ? 6.0f : 9.0f,
                            chosen ? palette::blurple : palette::sidebar);
                ui::text(over, fonts.semibold, i18n::language_name(static_cast<i18n::Lang>(i)),
                         row.x + 68, row.y + 40, 22, palette::header);
                if (settings_content_ && settings_row_ == i)
                    draw_focus_ring(over, row, 8);
                y += 76;
            }
            break;
        }
        case Section::sound:
        {
            const char *names[] = {"Arayüz sesleri", "Titreşim", "Işık çubuğu",
                                   "Onay için O, geri için X"};
            const bool flags[] = {settings.ui_volume > 0, settings.haptics, settings.light_bar,
                                  settings.swap_confirm};
            for (int i = 0; i < 4; ++i)
            {
                const Rect row{x, y, w, 72};
                ui::text(over, fonts.semibold, T(names[i]), row.x + 4, row.y + 44, 22,
                         palette::header);
                if (i == 0)
                {
                    const Rect bar{row.x + w - 260, row.y + 32, 200, 8};
                    over.rounded_rect(bar, 4, palette::rail);
                    over.rounded_rect({bar.x, bar.y, bar.w * settings.ui_volume / 10.0f, bar.h}, 4,
                                      palette::blurple);
                    over.circle(bar.x + bar.w * settings.ui_volume / 10.0f, bar.y + 4, 11,
                                palette::white);
                    ui::text(over, fonts.regular, std::to_string(settings.ui_volume * 10) + "%",
                             row.x + w - 4, row.y + 44, 20, palette::muted, gfx::Align::right);
                }
                else
                {
                    draw_switch(over, row.x + w - 4, row.y + 36, flags[i]);
                }
                over.line(row.x, row.y + row.h, row.x + w, row.y + row.h, 2, palette::divider);
                if (settings_content_ && settings_row_ == i)
                    draw_focus_ring(over, {row.x - 8, row.y + 4, row.w + 16, row.h - 8}, 8);
                y += 84;
            }
            break;
        }
        case Section::about:
        {
            if (version_.empty())
                version_ = read_content_version("/app0/sce_sys/param.json");
            const Rect card{x, y, w, 200};
            over.rounded_rect(card, 12, palette::sidebar);
            over.rounded_rect({card.x + 32, card.y + 40, 120, 120}, 28, palette::blurple);
            draw_mark(over, card.x + 92, card.y + 102, 2.4f, palette::white, palette::blurple);
            ui::text(over, fonts.semibold, "PSCord", card.x + 184, card.y + 88, 32,
                     palette::header);
            ui::text(over, fonts.regular,
                     std::string(T("Sürüm")) + " " + (version_.empty() ? "-" : version_),
                     card.x + 184, card.y + 126, 22, palette::normal);
            ui::text(over, fonts.regular, T("Güncellemeler uygulama açılırken otomatik kurulur."),
                     card.x + 184, card.y + 160, 18, palette::muted);
            const Rect join{x, y + 224, w, 88};
            over.rounded_rect(join, 12, palette::sidebar);
            ui::text(over, fonts.semibold, T("Topluluğa katıl"), join.x + 24, join.y + 36, 22,
                     palette::header);
            ui::text(over, fonts.regular, T("discord.gg/wfTch3qUa6"), join.x + 24, join.y + 66, 18,
                     palette::blurple);
            if (settings_content_ && settings_row_ == 0)
                draw_focus_ring(over, join, 12);
            const Rect coffee{x, y + 336, w, 248};
            over.rounded_rect(coffee, 12, palette::sidebar);
            ui::text(over, fonts.semibold, T("Bir kahve ısmarla"), coffee.x + 24, coffee.y + 48, 22,
                     palette::header);
            ui::text(over, fonts.regular, T("buymeacoffee.com/desstann"), coffee.x + 24,
                     coffee.y + 84, 18, palette::blurple);
            ui::text(over, fonts.regular, T("Beğendiysen telefonunla okut."), coffee.x + 24,
                     coffee.y + 124, 18, palette::muted);
            if (coffee_qr_.empty())
                coffee_qr_ = encode_qr("https://buymeacoffee.com/desstann", coffee_qr_size_);
            if (coffee_qr_size_ > 0)
                draw_qr(over, coffee_qr_, coffee_qr_size_,
                        {coffee.x + coffee.w - 216, coffee.y + 24, 200, 200}, 14);
            if (settings_content_ && settings_row_ == 1)
                draw_focus_ring(over, coffee, 12);
            break;
        }
        default:
            break;
        }
    }

    // ---- the client -----------------------------------------------------------

    void draw_app(gfx::DrawList &list) const
    {
        draw_rail(list);
        draw_sidebar(list);
        if (in_call_view())
        {
            draw_call(list);
            return;
        }
        draw_chat(list);
        if (show_members())
            draw_members(list);
    }

    float rail_center(int index) const
    {
        const float first = layout::icon_gap + layout::icon * 0.5f;
        return first + static_cast<float>(index) * (layout::icon + layout::icon_gap) +
               (index >= 1 ? 16.0f : 0.0f);
    }

    void draw_rail(gfx::DrawList &list) const
    {
        const auto &fonts = context_.fonts;
        list.rounded_rect({0, 0, layout::rail_w, 1080}, 0, palette::rail);
        const float offset = std::max(0.0f, rail_center(guild_) + layout::icon - 1080);
        const float cx = layout::rail_w * 0.5f;
        const bool focused = column_ == Column::guilds;

        list.push_clip({0, 0, layout::rail_w, 1080});
        for (int i = 0; i < static_cast<int>(snap_.guilds.size()); ++i)
        {
            const float cy = rail_center(i) - offset;
            if (cy < -layout::icon || cy > 1080 + layout::icon)
                continue;
            const dc::Guild &g = snap_.guilds[static_cast<std::size_t>(i)];
            const bool active = i == guild_;
            const Rect icon{cx - layout::icon * 0.5f, cy - layout::icon * 0.5f, layout::icon,
                            layout::icon};
            const float radius = active ? 24.0f : layout::icon * 0.5f;
            if (active)
                list.rounded_rect({0, cy - 30, 6, 60}, 3, palette::white);
            else if (g.unread)
                list.rounded_rect({0, cy - 6, 6, 12}, 3, palette::white);
            if (g.id.empty())
            {
                list.rounded_rect(icon, radius, active ? palette::blurple : palette::chat);
                draw_mark(list, cx, cy + 2, 1.0f, active ? palette::white : palette::normal,
                          active ? palette::blurple : palette::chat);
            }
            else if (const std::uint32_t tex = images_.get(g.icon_url, 128, 128, clock_))
            {
                list.image(tex, icon, gfx::kFullUv, palette::white, radius);
            }
            else
            {
                list.rounded_rect(icon, radius, active ? palette::blurple : palette::chat);
                const std::string text = initials(g.name);
                const float size = text.size() > 3 ? 20.0f : 26.0f;
                ui::text(list, fonts.semibold, text, cx, cy + size * 0.36f, size,
                         active ? palette::white : palette::normal, gfx::Align::center);
            }
            if (active && focused)
                draw_focus_ring(list, icon, radius);
            if (g.mentions > 0)
                draw_badge(list, fonts, icon.x + icon.w + 4, icon.y + icon.h - 8, g.mentions,
                           palette::rail);
            if (i == 0)
                list.line(cx - 24, cy + layout::icon * 0.5f + 14, cx + 24,
                          cy + layout::icon * 0.5f + 14, 2, palette::hover);
        }
        list.pop_clip();
    }

    void draw_sidebar(gfx::DrawList &list) const
    {
        const auto &fonts = context_.fonts;
        const float x = layout::sidebar_x;
        const float w = layout::sidebar_w;
        list.rounded_rect({x, 0, w, 1080}, 0, palette::sidebar);

        const bool dms = guild_ == 0;
        if (dms)
        {
            list.rounded_rect({x + 16, 15, w - 32, 42}, 6, palette::rail);
            ui::text(list, fonts.regular, T("Sohbet bul ya da başlat"), x + 28, 43, 20,
                     palette::muted);
        }
        else
        {
            const std::string title = guild_ < static_cast<int>(snap_.guilds.size())
                                          ? snap_.guilds[static_cast<std::size_t>(guild_)].name
                                          : std::string();
            label(list, fonts.semibold, title, x + 24, 46, 24, palette::header, w - 80);
            list.line(x + w - 38, 34, x + w - 32, 40, 2.5f, palette::header);
            list.line(x + w - 32, 40, x + w - 26, 34, 2.5f, palette::header);
        }
        list.line(x, layout::header_h, x + w, layout::header_h, 2, palette::divider);

        const auto &items = channels();
        const bool focused = column_ == Column::channels;
        const float top = layout::header_h + 12;
        const float bottom = 1080 - layout::panel_h - voice_panel_height() - 8;
        auto row_height = [&](const dc::Channel &c)
        {
            if (c.type == 4)
                return layout::category_h;
            return (dms ? layout::dm_row_h : layout::row_h) +
                   layout::voice_user_h * static_cast<float>(c.voice_users.size());
        };
        const voice::Status call = voice::bg::live(voice::Engine::instance().status());

        float cursor_y = top + (dms ? 44.0f : 0.0f);
        float cursor_h = layout::dm_row_h;
        float y = cursor_y;
        for (int i = 0; i < static_cast<int>(items.size()); ++i)
        {
            const float h = row_height(items[static_cast<std::size_t>(i)]);
            if (i == channel_)
            {
                cursor_y = y;
                cursor_h = std::max(h, layout::dm_row_h);
            }
            y += h;
        }
        const float offset = std::max(0.0f, cursor_y + cursor_h + 16 - bottom);

        list.push_clip({x, layout::header_h + 2, w, bottom - layout::header_h - 2});
        y = top - offset;
        if (dms)
        {
            ui::text(list, fonts.semibold, T("DİREKT MESAJLAR"), x + 24, y + 30, 18,
                     palette::muted);
            y += 44;
        }
        for (int i = 0; i < static_cast<int>(items.size()); ++i)
        {
            const dc::Channel &c = items[static_cast<std::size_t>(i)];
            const float h = row_height(c);
            if (y + h < layout::header_h || y > bottom)
            {
                y += h;
                continue;
            }
            if (c.type == 4)
            {
                const float cy = y + 36;
                list.line(x + 12, cy - 4, x + 17, cy + 1, 2, palette::muted);
                list.line(x + 17, cy + 1, x + 22, cy - 4, 2, palette::muted);
                label(list, fonts.semibold, upper_tr(c.name), x + 30, cy + 6, 18, palette::muted,
                      w - 60);
            }
            else
            {
                const float base =
                    h - layout::voice_user_h * static_cast<float>(c.voice_users.size());
                draw_channel_row(list, c, {x + 12, y + 2, w - 24, base - 4},
                                 i == channel_ && focused);
                float uy = y + base;
                for (const dc::VoiceUser &u : c.voice_users)
                {
                    const float cy = uy + layout::voice_user_h * 0.5f;
                    const bool talking = call.speaking.count(u.id) > 0;
                    const std::string name = u.name.empty() ? std::string("…") : u.name;
                    if (talking)
                        list.ring(x + 66, cy, 17, 2.5f, palette::green);
                    draw_avatar(list, u.avatar_url, x + 66, cy, 14, u.id, name);
                    label(list, fonts.regular, name, x + 92, cy + 7, 21,
                          talking ? palette::header : palette::muted, w - 116);
                    uy += layout::voice_user_h;
                }
            }
            y += h;
        }
        list.pop_clip();

        draw_voice_panel(list, call);
        draw_user_panel(list);
    }

    void draw_channel_row(gfx::DrawList &list, const dc::Channel &c, const Rect &row,
                          bool cursor) const
    {
        const auto &fonts = context_.fonts;
        const bool open = c.id == snap_.channel_id;
        if (open || cursor)
            list.rounded_rect(row, 6, open ? palette::selected : palette::hover);
        if (cursor)
            draw_focus_ring(list, row, 6);
        const bool bright = open || cursor || c.unread;
        const Color text = bright ? palette::header : c.muted ? palette::dim : palette::muted;
        const float cy = row.y + row.h * 0.5f;
        const float baseline = cy + 8.5f;
        float right = row.x + row.w - 12;
        if (c.unread && !open && !c.muted)
            list.rounded_rect({layout::sidebar_x - 4, cy - 6, 8, 12}, 4, palette::white);
        if (c.mentions > 0)
        {
            draw_badge(list, fonts, right, cy, c.mentions, palette::clear);
            right -= 40;
        }
        float name_x;
        if (is_dm(c))
        {
            draw_avatar(list, c.icon_url, row.x + 30, cy, 24, c.id, c.name);
            name_x = row.x + 66;
        }
        else
        {
            discord::draw_channel_icon(list, c.type, c.rules, row.x + 8, cy, 30,
                                       bright ? palette::interactive : palette::faint);
            name_x = row.x + 48;
        }
        label(list, fonts.semibold, c.name, name_x, baseline, 24, text, right - name_x);
    }

    float voice_panel_height() const
    {
        return snap_.voice_channel_id.empty() ? 0.0f : layout::voice_panel_h;
    }

    // Discord's "Voice Connected" box: signal bars, state, channel / server, leave.
    void draw_voice_panel(gfx::DrawList &list, const voice::Status &call) const
    {
        if (snap_.voice_channel_id.empty())
            return;
        const auto &fonts = context_.fonts;
        const float x = layout::sidebar_x;
        const float w = layout::sidebar_w;
        const float y = 1080 - layout::panel_h - layout::voice_panel_h;
        list.rounded_rect({x, y, w, layout::voice_panel_h}, 0, palette::panel);
        list.line(x + 16, y + layout::voice_panel_h - 1, x + w - 16, y + layout::voice_panel_h - 1,
                  2, palette::divider);

        const bool connected = call.phase == voice::Phase::connected;
        const bool failed = call.phase == voice::Phase::failed;
        const Color tone = connected ? palette::green
                           : failed  ? palette::red
                                     : Color::rgb(0xf0b232);
        for (int bar = 0; bar < 3; ++bar)
        {
            const float bh = 8.0f + 6.0f * static_cast<float>(bar);
            list.rounded_rect({x + 22 + 8.0f * static_cast<float>(bar), y + 44 - bh, 5, bh}, 2,
                              tone);
        }
        const char *title = connected ? T("Ses Bağlandı")
                            : failed  ? T("Ses Bağlantısı Kesildi")
                                      : T("Bağlanıyor");
        ui::text(list, fonts.semibold, title, x + 56, y + 44, 22, tone);

        std::string where;
        for (const dc::Guild &g : snap_.guilds)
            for (const dc::Channel &c : g.channels)
                if (c.id == snap_.voice_channel_id)
                    where = c.name + " / " + g.name;
        const std::string detail = failed ? call.text : (call.e2ee ? "🔒 " : "") + where;
        const ui::GlyphStyle glyphs = ui::GlyphStyle::dark();
        const float button_w = ui::button_width(ui::Button::triangle, 28);
        const float button_x = x + w - 16 - button_w;
        label(list, fonts.regular, detail, x + 22, y + 78, 19, palette::muted, button_x - x - 40);
        ui::draw_button(list, fonts, glyphs, ui::Button::triangle, button_x,
                        y + layout::voice_panel_h * 0.5f, 28);
    }

    void draw_user_panel(gfx::DrawList &list) const
    {
        const auto &fonts = context_.fonts;
        const float x = layout::sidebar_x;
        const float y = 1080 - layout::panel_h;
        list.rounded_rect({x, y, layout::sidebar_w, layout::panel_h}, 0, palette::panel);
        const float cx = x + 16 + 24;
        const float cy = y + layout::panel_h * 0.5f;
        draw_avatar(list, snap_.me_avatar, cx, cy, 24, snap_.me_id, snap_.me);
        list.circle(cx + 17, cy + 17, 10, palette::panel);
        list.circle(cx + 17, cy + 17, 6, palette::green);

        const ui::GlyphStyle glyphs = ui::GlyphStyle::dark();
        const float button_w = ui::button_width(ui::Button::options, 28);
        const float button_x = x + layout::sidebar_w - 16 - button_w;
        label(list, fonts.semibold, snap_.me, x + 80, cy - 4, 22, palette::header,
              button_x - (x + 80) - 12);
        ui::text(list, fonts.regular, T("Çevrim içi"), x + 80, cy + 24, 18, palette::muted);
        ui::draw_button(list, fonts, glyphs, ui::Button::options, button_x, cy, 28);
    }

    void draw_header(gfx::DrawList &list, const dc::Channel *open) const
    {
        const auto &fonts = context_.fonts;
        const ui::Hint hints[] = {
            {ui::Button::cross, T("Aç")},
            {ui::Button::circle, T("Geri")},
            {ui::Button::l1, T("Sunucu"), ui::Button::r1},
            {ui::Button::square, T("Mesaj")},
            {ui::Button::options, T("Ayarlar")},
            {ui::Button::triangle, T("Sesten Ayrıl")},
        };
        const int hint_count = snap_.voice_channel_id.empty() ? 5 : 6;
        ui::HintLayout hint_layout;
        hint_layout.size = 30;
        hint_layout.text_size = 20;
        hint_layout.cy = 36;
        hint_layout.item_gap = 28;
        ui::GlyphStyle style = ui::GlyphStyle::dark();
        style.label = palette::muted;
        const float hints_w = ui::measure_hints(fonts, hints, hint_count, hint_layout);
        ui::draw_hints(list, fonts, style, hints, hint_count, 1920 - 24, true, hint_layout);
        list.line(layout::chat_x, layout::header_h, 1920, layout::header_h, 2, palette::divider);
        if (!open)
            return;

        float x = layout::chat_x + 24;
        if (is_dm(*open))
        {
            draw_avatar(list, open->icon_url, x + 18, 36, 18, open->id, open->name);
            x += 48;
        }
        else
        {
            discord::draw_channel_icon(list, open->type, open->rules, x, 36, 36, palette::faint);
            x += 46;
        }
        const float limit = 1920 - 24 - hints_w - 32;
        x += label(list, fonts.semibold, open->name, x, 45, 24, palette::header,
                   std::min(600.0f, limit - x));
        // Inside a post the header reads "forum > post".
        if (in_post() && x + 80 < limit)
        {
            ui::text(list, fonts.regular, "›", x + 20, 45, 24, palette::muted);
            x += 44;
            x += label(list, fonts.semibold, open_post_name(), x, 45, 24, palette::header,
                       limit - x);
            return;
        }
        if (!open->topic.empty() && x + 80 < limit)
        {
            list.line(x + 20, 24, x + 20, 48, 2, palette::separator);
            std::string topic = open->topic;
            std::replace(topic.begin(), topic.end(), '\n', ' ');
            label(list, fonts.regular, topic, x + 40, 44, 21, palette::muted, limit - x - 40);
        }
    }

    void draw_chat(gfx::DrawList &list) const
    {
        const auto &fonts = context_.fonts;
        const float right = chat_right();
        list.rounded_rect({layout::chat_x, 0, 1920 - layout::chat_x, 1080}, 0, palette::chat);
        const dc::Channel *open = open_channel();
        draw_header(list, open);

        const float center = (layout::chat_x + right) * 0.5f;
        if (!open)
        {
            draw_mark(list, center, 470, 3.0f, palette::selected, palette::chat);
            ui::text(list, fonts.semibold, T("Bir kanal seç"), center, 600, 32, palette::header,
                     gfx::Align::center);
            ui::text(list, fonts.regular, T("Soldan bir sunucu ve kanal seçip aç."), center, 644,
                     24, palette::muted, gfx::Align::center);
            return;
        }
        if (in_post_list())
        {
            draw_posts(list, *open);
            return;
        }
        draw_messages(list, *open);
        draw_composer(list, *open);
    }

    // The posts of a forum: title, who opened it, the first line and the reply count.
    void draw_posts(gfx::DrawList &list, const dc::Channel &forum) const
    {
        const auto &fonts = context_.fonts;
        const float right = chat_right();
        const float center = (layout::chat_x + right) * 0.5f;
        const std::vector<dc::ForumPost> &all = forum.posts;
        if (all.empty())
        {
            const char *line = snap_.loading_posts           ? "Gönderiler yükleniyor..."
                               : snap_.channel_error.empty() ? "Bu forumda henüz gönderi yok."
                                                             : nullptr;
            discord::draw_channel_icon(list, forum.type, false, center - 48, 470, 96,
                                       palette::selected);
            ui::text(list, fonts.semibold, T("Forum gönderileri"), center, 600, 32, palette::header,
                     gfx::Align::center);
            ui::text(list, fonts.regular, line ? T(line) : snap_.channel_error, center, 644, 24,
                     palette::muted, gfx::Align::center);
            return;
        }

        const float x = layout::text_x;
        const float w = right - x - 36;
        const float row_h = 132;
        const float top = layout::header_h + 24;
        const int fits = std::max(1, static_cast<int>((1080 - top - 24) / row_h));
        const int count = static_cast<int>(all.size());
        const int first = std::clamp(post_ - fits / 2, 0, std::max(0, count - fits));
        for (int i = first; i < std::min(count, first + fits); ++i)
        {
            const dc::ForumPost &p = all[static_cast<std::size_t>(i)];
            const Rect row{x, top + (i - first) * row_h, w, row_h - 16};
            const bool focused = column_ == Column::messages && i == post_;
            list.rounded_rect(row, 10, focused ? palette::selected : palette::sidebar);
            label(list, fonts.semibold, p.name, row.x + 24, row.y + 42, 24, palette::header,
                  row.w - 48);
            if (!p.excerpt.empty())
            {
                std::string line = p.excerpt;
                std::replace(line.begin(), line.end(), '\n', ' ');
                label(list, fonts.regular, line, row.x + 24, row.y + 78, 20, palette::normal,
                      row.w - 48);
            }
            std::string foot = p.author.empty() ? std::string() : p.author;
            if (p.timestamp > 0)
                foot += (foot.empty() ? "" : " · ") + day_label(p.timestamp);
            if (p.message_count > 0)
                foot +=
                    (foot.empty() ? "" : " · ") + T("%s yanıt", std::to_string(p.message_count));
            if (!foot.empty())
                label(list, fonts.regular, foot, row.x + 24, row.y + 104, 18, palette::muted,
                      row.w - 48);
            if (focused)
                draw_focus_ring(list, row, 10);
        }
        if (count > fits)
        {
            ui::text(list, fonts.regular, T("%s gönderi", std::to_string(count)), right - 36,
                     1080 - 28, 18, palette::muted, gfx::Align::right);
        }
    }

    // ---- messages -----------------------------------------------------------

    float content_width() const
    {
        return chat_right() - layout::text_x - 36;
    }

    static bool starts_group(const dc::Message &m, const dc::Message *previous)
    {
        return !previous || previous->author_id != m.author_id || m.reply || is_system(m.type) ||
               is_system(previous->type) || m.timestamp - previous->timestamp > 420 ||
               day_key(m.timestamp) != day_key(previous->timestamp);
    }

    // The text a message shows, unless it is only the link of the GIF or picture it embeds.
    static bool content_visible(const dc::Message &m)
    {
        if (m.content.empty())
            return false;
        std::string trimmed = m.content;
        while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '\n'))
            trimmed.pop_back();
        for (const dc::Embed &e : m.embeds)
            if ((e.type == "gifv" || e.type == "image") && e.url == trimmed)
                return false;
        return true;
    }

    // Draws (list set) or measures (list null) one message from its top y. Returns its height.
    float message_block(gfx::DrawList *list, const dc::Message &m, bool header, float y) const
    {
        const auto &fonts = context_.fonts;
        const float width = content_width();
        const float x = layout::text_x;
        float cy = y + (header ? 24.0f : 3.0f);

        if (is_system(m.type))
        {
            std::string what;
            switch (m.type)
            {
            case 7:
                what = T(" sunucuya katıldı.");
                break;
            case 6:
                what = T(" bu kanala bir mesaj sabitledi.");
                break;
            case 8:
            case 9:
            case 10:
            case 11:
                what = T(" sunucuya takviye yaptı!");
                break;
            case 18:
                what = T(" bir alt başlık başlattı: ") + m.content;
                break;
            default:
                what = m.content.empty() ? T(" bir işlem yaptı.") : " " + m.content;
                break;
            }
            if (list)
            {
                const Color arrow = m.type == 7 ? palette::green : palette::blurple;
                list->line(layout::avatar_cx - 12, cy + 18, layout::avatar_cx + 10, cy + 18, 3,
                           arrow);
                list->line(layout::avatar_cx + 2, cy + 10, layout::avatar_cx + 10, cy + 18, 3,
                           arrow);
                list->line(layout::avatar_cx + 2, cy + 26, layout::avatar_cx + 10, cy + 18, 3,
                           arrow);
                const float name_w = label(*list, fonts.semibold, m.author, x, cy + 26, 24,
                                           discord::role_color(m.color, palette::header), width);
                const float what_w = label(*list, fonts.regular, what, x + name_w, cy + 26, 24,
                                           palette::muted, width - name_w);
                ui::text(*list, fonts.regular, time_label(m.timestamp), x + name_w + what_w + 12,
                         cy + 26, 18, palette::muted);
            }
            return cy + 36 - y;
        }

        if (m.reply)
        {
            if (list)
            {
                const float spine_x = layout::avatar_cx;
                list->line(spine_x, cy + 34, spine_x, cy + 20, 3, palette::dim);
                list->line(spine_x - 1.5f, cy + 18.5f, x - 10, cy + 18.5f, 3, palette::dim);
                float rx = x;
                if (!m.reply_author.empty())
                {
                    draw_avatar(*list, m.reply_avatar, rx + 12, cy + 18, 12, m.reply_author_id,
                                m.reply_author);
                    rx += 30;
                    rx += label(*list, fonts.semibold, "@" + m.reply_author, rx, cy + 25, 20,
                                discord::role_color(m.reply_color, palette::header), 300) +
                          8;
                }
                std::string preview = m.reply_content;
                std::replace(preview.begin(), preview.end(), '\n', ' ');
                label(*list, fonts.regular, preview, rx, cy + 25, 20, palette::interactive,
                      x + width - rx);
            }
            cy += 36;
        }

        if (header)
        {
            if (list)
            {
                draw_avatar(*list, m.avatar_url, layout::avatar_cx, cy + layout::avatar * 0.5f,
                            layout::avatar * 0.5f, m.author_id, m.author);
                float nx = x;
                nx += label(*list, fonts.semibold, m.author, nx, cy + 26, 24,
                            discord::role_color(m.color, palette::header), width * 0.6f);
                if (m.bot)
                {
                    draw_bot_tag(*list, nx + 8, cy + 25);
                    nx += 8 + fonts.semibold.measure("UYG", 15) + 12;
                }
                ui::text(*list, fonts.regular, time_label(m.timestamp), nx + 12, cy + 25, 18,
                         palette::muted);
            }
            cy += 34;
        }

        if (content_visible(m) || (m.edited && !m.content.empty()))
        {
            const RichText &text =
                rich(m.id + ":c", m.content, width, 24, palette::normal, &m, m.edited);
            if (list)
                discord::draw_rich(*list, fonts, text, x, cy, images_, clock_);
            cy += text.height;
        }

        for (const dc::Attachment &a : m.attachments)
        {
            cy += 6;
            if (!a.media.url.empty())
            {
                if (list)
                    draw_media(*list, a.media, x, cy);
                cy += static_cast<float>(a.media.height) + 6;
                continue;
            }
            const Rect card{x, cy, std::min(width, 600.0f), 96};
            if (list)
            {
                list->bordered_rect(card, 12, palette::code_bg, 1.5f, palette::code_border);
                list->rounded_rect({card.x + 24, card.y + 21, 40, 54}, 6, palette::blurple);
                list->rounded_rect({card.x + 32, card.y + 40, 24, 3}, 1,
                                   palette::white.with_alpha(0.8f));
                list->rounded_rect({card.x + 32, card.y + 49, 24, 3}, 1,
                                   palette::white.with_alpha(0.8f));
                list->rounded_rect({card.x + 32, card.y + 58, 16, 3}, 1,
                                   palette::white.with_alpha(0.8f));
                label(*list, fonts.regular, a.filename, card.x + 84, card.y + 44, 24, palette::link,
                      card.w - 108);
                ui::text(*list, fonts.regular, file_size(a.size), card.x + 84, card.y + 72, 18,
                         palette::muted);
            }
            cy += card.h + 6;
        }

        for (const dc::Sticker &s : m.stickers)
        {
            cy += 6;
            if (s.url.empty())
            {
                if (list)
                    label(*list, fonts.regular, "[Çıkartma: " + s.name + "]", x, cy + 26, 22,
                          palette::muted, width);
                cy += 36;
                continue;
            }
            if (list)
            {
                if (const std::uint32_t tex = images_.get(s.url, 240, 240, clock_))
                    list->image(tex, {x, cy, 240, 240}, gfx::kFullUv, palette::white);
            }
            cy += 246;
        }

        for (std::size_t i = 0; i < m.embeds.size(); ++i)
        {
            const dc::Embed &e = m.embeds[i];
            const bool bare = (e.type == "gifv" || e.type == "image") && !e.image.url.empty();
            cy += 6;
            if (bare)
            {
                if (list)
                    draw_media(*list, e.image, x, cy);
                cy += static_cast<float>(e.image.height) + 6;
                continue;
            }
            cy += embed_block(list, m, e, m.id + ":e" + std::to_string(i), x, cy, width) + 6;
        }

        if (!m.reactions.empty())
            cy += reactions_block(list, m, x, cy + 6, width) + 6;

        return cy + 4 - y;
    }

    float embed_block(gfx::DrawList *list, const dc::Message &m, const dc::Embed &e,
                      const std::string &key, float x, float y, float width) const
    {
        const auto &fonts = context_.fonts;
        float card_w = std::min(width, 720.0f);
        if (!e.image.url.empty())
            card_w = std::min(width, std::max(480.0f, static_cast<float>(e.image.width) + 48));
        const float pad = 18;
        const bool thumb = !e.thumbnail.url.empty();
        const float inner_x = x + 6 + pad;
        const float full_w = card_w - 6 - pad * 2;
        const float text_w = full_w - (thumb ? static_cast<float>(e.thumbnail.width) + 24 : 0.0f);

        if (list)
        {
            const float h = embed_block(nullptr, m, e, key, x, y, width);
            list->rounded_rect({x, y, card_w, h}, 6, palette::code_bg);
            list->rounded_rect({x, y, 6, h}, 3,
                               e.has_color ? Color::rgb(e.color) : palette::code_border);
        }
        float cy = y + pad;
        if (!e.provider.empty())
        {
            if (list)
                label(*list, fonts.regular, e.provider, inner_x, cy + 18, 18, palette::interactive,
                      text_w);
            cy += 30;
        }
        if (!e.author.empty())
        {
            float ax = inner_x;
            if (list)
            {
                if (!e.author_icon.empty())
                {
                    if (const std::uint32_t tex = images_.get(e.author_icon, 36, 36, clock_))
                        list->image(tex, {ax, cy, 36, 36}, gfx::kFullUv, palette::white, 18);
                    ax += 48;
                }
                label(*list, fonts.semibold, e.author, ax, cy + 26, 21, palette::header,
                      text_w - (ax - inner_x));
            }
            cy += 42;
        }
        if (!e.title.empty())
        {
            const RichText &text = rich(key + ":t", "**" + e.title + "**", text_w, 24,
                                        e.url.empty() ? palette::header : palette::link, &m);
            if (list)
                discord::draw_rich(*list, fonts, text, inner_x, cy, images_, clock_);
            cy += text.height + 6;
        }
        if (!e.description.empty())
        {
            const RichText &text = rich(key + ":d", e.description, text_w, 21, palette::normal, &m);
            if (list)
                discord::draw_rich(*list, fonts, text, inner_x, cy, images_, clock_);
            cy += text.height + 6;
        }
        for (std::size_t i = 0; i < e.fields.size();)
        {
            std::size_t n = 1;
            if (e.fields[i].is_inline)
                while (n < 3 && i + n < e.fields.size() && e.fields[i + n].is_inline)
                    ++n;
            const float col_w = (text_w - static_cast<float>(n - 1) * 24) / static_cast<float>(n);
            float row_h = 0;
            for (std::size_t k = 0; k < n; ++k)
            {
                const dc::EmbedField &f = e.fields[i + k];
                const float fx = inner_x + static_cast<float>(k) * (col_w + 24);
                const std::string fkey = key + ":f" + std::to_string(i + k);
                const RichText &name =
                    rich(fkey + "n", "**" + f.name + "**", col_w, 21, palette::header, &m);
                const RichText &value = rich(fkey + "v", f.value, col_w, 21, palette::normal, &m);
                if (list)
                {
                    discord::draw_rich(*list, fonts, name, fx, cy, images_, clock_);
                    discord::draw_rich(*list, fonts, value, fx, cy + name.height, images_, clock_);
                }
                row_h = std::max(row_h, name.height + value.height);
            }
            cy += row_h + 12;
            i += n;
        }
        if (!e.image.url.empty())
        {
            dc::MediaRef media = e.image;
            if (static_cast<float>(media.width) > full_w)
            {
                const float scale = full_w / static_cast<float>(media.width);
                media.width = static_cast<int>(full_w);
                media.height = static_cast<int>(static_cast<float>(media.height) * scale);
            }
            cy += 6;
            if (list)
                draw_media(*list, media, inner_x, cy);
            cy += static_cast<float>(media.height) + 6;
        }
        if (!e.footer.empty())
        {
            if (list)
                label(*list, fonts.regular, e.footer, inner_x, cy + 20, 18, palette::interactive,
                      full_w);
            cy += 30;
        }
        float h = cy - y + pad - 6;
        if (thumb)
        {
            if (list)
                draw_media(*list, e.thumbnail,
                           x + card_w - pad - static_cast<float>(e.thumbnail.width), y + pad);
            h = std::max(h, static_cast<float>(e.thumbnail.height) + pad * 2);
        }
        return h;
    }

    float reactions_block(gfx::DrawList *list, const dc::Message &m, float x, float y,
                          float width) const
    {
        const auto &fonts = context_.fonts;
        float cx = x;
        float cy = y;
        for (const dc::Reaction &r : m.reactions)
        {
            const std::string count = std::to_string(r.count);
            const float w = 12 + 27 + 9 + fonts.semibold.measure(count, 20) + 12;
            if (cx + w > x + width && cx > x)
            {
                cx = x;
                cy += 42;
            }
            if (list)
            {
                const Rect pill{cx, cy, w, 36};
                if (r.me)
                    list->bordered_rect(pill, 12, palette::blurple.with_alpha(0.15f), 1.5f,
                                        palette::blurple);
                else
                    list->rounded_rect(pill, 12, palette::code_bg);
                if (const std::uint32_t tex = images_.get(r.emoji_url, 48, 48, clock_))
                    list->image(tex, {cx + 12, cy + 4.5f, 27, 27}, gfx::kFullUv, palette::white);
                ui::text(*list, fonts.semibold, count, cx + 12 + 27 + 9, cy + 25, 20,
                         r.me ? Color::rgb(0xdee0fc) : palette::interactive);
            }
            cx += w + 6;
        }
        return cy + 36 - y;
    }

    void draw_divider(gfx::DrawList &list, float y, std::int64_t ts) const
    {
        const auto &fonts = context_.fonts;
        const float left = layout::chat_x + 24;
        const float right = chat_right() - 24;
        const float mid = (left + right) * 0.5f;
        const std::string text = day_label(ts);
        const float w = fonts.semibold.measure(text, 18) + 24;
        list.line(left, y, right, y, 1.5f, palette::separator);
        list.rounded_rect({mid - w * 0.5f, y - 12, w, 24}, 0, palette::chat);
        ui::text(list, fonts.semibold, text, mid, y + 6, 18, palette::muted, gfx::Align::center);
    }

    void draw_messages(gfx::DrawList &list, const dc::Channel &open) const
    {
        const auto &fonts = context_.fonts;
        const float top = layout::header_h + 2;
        const float bottom = layout::composer_y - 12;
        const float right = chat_right();
        const float center = (layout::chat_x + right) * 0.5f;

        if (snap_.loading_messages && snap_.messages.empty())
        {
            ui::text(list, fonts.regular, T("Mesajlar yükleniyor..."), center, 540, 24,
                     palette::muted, gfx::Align::center);
            return;
        }
        if (!snap_.channel_error.empty())
            ui::text(list, fonts.regular, snap_.channel_error, center, bottom - 8, 22, palette::red,
                     gfx::Align::center);
        if (snap_.messages.empty())
        {
            draw_channel_start(list, open, bottom);
            return;
        }

        list.push_clip({layout::chat_x, top, right - layout::chat_x, bottom - top});
        float y = bottom;
        const int last = static_cast<int>(snap_.messages.size()) - 1 - scroll_;
        int i = last;
        for (; i >= 0 && y > top; --i)
        {
            const dc::Message &m = snap_.messages[static_cast<std::size_t>(i)];
            const dc::Message *previous =
                i > 0 ? &snap_.messages[static_cast<std::size_t>(i - 1)] : nullptr;
            const bool header = starts_group(m, previous);
            const bool divider = !previous || day_key(previous->timestamp) != day_key(m.timestamp);
            const float h = message_block(nullptr, m, header, 0);
            y -= h;
            if (i == last && column_ == Column::messages)
                list.rounded_rect({layout::chat_x, y + (header ? 16.0f : 0.0f),
                                   right - layout::chat_x, h - (header ? 16.0f : 0.0f)},
                                  0, palette::message_hover);
            message_block(&list, m, header, y);
            if (divider)
            {
                y -= 36;
                draw_divider(list, y + 24, m.timestamp);
            }
        }
        if (i < 0 && snap_.messages.size() < 50 && y > top + 260)
            draw_channel_start(list, open, y - 36);
        list.pop_clip();
    }

    // What Discord shows above the first message of a channel.
    void draw_channel_start(gfx::DrawList &list, const dc::Channel &open, float bottom) const
    {
        const auto &fonts = context_.fonts;
        const float x = layout::chat_x + 24;
        const bool dm = is_dm(open);
        float y = bottom - 180;
        if (dm)
            draw_avatar(list, open.icon_url, x + 60, y, 60, open.id, open.name);
        else
        {
            list.circle(x + 51, y, 51, palette::selected);
            discord::draw_channel_icon(list, open.type, open.rules, x + 21, y, 60, palette::header);
        }
        y += 100;
        const std::string title = dm ? open.name : T("%s kanalına hoş geldin!", open.name);
        float tx = x;
        if (!dm)
        {
            discord::draw_channel_icon(list, open.type, open.rules, x, y - 14, 44, palette::header);
            tx += 48;
        }
        label(list, fonts.semibold, title, tx, y, 40, palette::header, chat_right() - tx - 24);
        const std::string sub = dm ? T("%s ile olan direkt mesaj geçmişinin başlangıcı.", open.name)
                                   : T("Bu, #%s kanalının başlangıcı.", open.name);
        label(list, fonts.regular, sub, x, y + 44, 24, palette::muted, chat_right() - x - 24);
    }

    void draw_composer(gfx::DrawList &list, const dc::Channel &open) const
    {
        const auto &fonts = context_.fonts;
        const Rect box{layout::chat_x + 24, layout::composer_y, chat_right() - layout::chat_x - 48,
                       layout::composer_h};
        list.rounded_rect(box, 12, palette::input);
        const float cy = box.y + box.h * 0.5f;
        list.circle(box.x + 36, cy, 18, palette::interactive);
        list.line(box.x + 28, cy, box.x + 44, cy, 3, palette::input);
        list.line(box.x + 36, cy - 8, box.x + 36, cy + 8, 3, palette::input);

        // Right side: gift, GIF, sticker and emoji, as on the desktop.
        float rx = box.x + box.w - 24;
        list.ring(rx - 14, cy, 14, 2.5f, palette::interactive);
        list.circle(rx - 19, cy - 4, 2.2f, palette::interactive);
        list.circle(rx - 9, cy - 4, 2.2f, palette::interactive);
        list.line(rx - 20, cy + 5, rx - 8, cy + 5, 2.5f, palette::interactive);
        rx -= 48;
        list.bordered_rect({rx - 28, cy - 14, 28, 28}, 7, palette::clear, 2.5f,
                           palette::interactive);
        list.line(rx - 10, cy + 14, rx, cy + 4, 2.5f, palette::interactive);
        rx -= 48;
        list.bordered_rect({rx - 34, cy - 12, 34, 24}, 5, palette::clear, 2.5f,
                           palette::interactive);
        ui::text(list, fonts.semibold, "GIF", rx - 17, cy + 5.5f, 14, palette::interactive,
                 gfx::Align::center);
        rx -= 48;
        list.bordered_rect({rx - 28, cy - 10, 28, 24}, 4, palette::clear, 2.5f,
                           palette::interactive);
        list.line(rx - 14, cy - 10, rx - 14, cy + 14, 2.5f, palette::interactive);
        list.line(rx - 28, cy - 2, rx, cy - 2, 2.5f, palette::interactive);
        rx -= 52;
        const float glyph_w = ui::button_width(ui::Button::square, 30);
        ui::draw_button(list, fonts, ui::GlyphStyle::dark(), ui::Button::square, rx - glyph_w, cy,
                        30);
        rx -= glyph_w + 16;

        const std::string hint = is_dm(open) ? T("@%s kullanıcısına mesaj gönder", open.name)
                                             : T("#%s kanalına mesaj gönder", open.name);
        label(list, fonts.regular, hint, box.x + 72, cy + 9, 24, palette::placeholder,
              rx - box.x - 72);
    }

    // ---- members --------------------------------------------------------------

    void draw_members(gfx::DrawList &list) const
    {
        const auto &fonts = context_.fonts;
        const float x = 1920 - layout::members_w;
        const float w = layout::members_w;
        list.rounded_rect({x, layout::header_h + 1, w, 1080 - layout::header_h}, 0,
                          palette::sidebar);
        list.push_clip({x, layout::header_h + 2, w, 1080 - layout::header_h});
        float y = layout::header_h + 12;
        for (const dc::Member &m : snap_.members)
        {
            if (y > 1080)
                break;
            if (m.group)
            {
                y += 24;
                label(list, fonts.semibold,
                      upper_tr(T(m.label.c_str())) + " — " + std::to_string(m.count), x + 24,
                      y + 18, 18, palette::muted, w - 48);
                y += 36;
                continue;
            }
            const bool offline = m.status == "offline";
            const float cy = y + 33;
            if (offline)
                list.push_opacity(0.3f);
            draw_avatar(list, m.avatar_url, x + 24 + 24, cy, 24, m.id, m.label);
            if (!offline)
            {
                const Color dot = m.status == "idle"  ? palette::idle
                                  : m.status == "dnd" ? palette::red
                                                      : palette::green;
                list.circle(x + 24 + 24 + 17, cy + 17, 10, palette::sidebar);
                list.circle(x + 24 + 24 + 17, cy + 17, 6, dot);
            }
            float nx = x + 24 + 48 + 18;
            nx += label(list, fonts.semibold, m.label, nx, cy + 8.5f, 24,
                        discord::role_color(m.color, palette::interactive),
                        w - (nx - x) - 24 - (m.bot ? 52.0f : 0.0f));
            if (m.bot)
                draw_bot_tag(list, nx + 8, cy + 8);
            if (offline)
                list.pop_opacity();
            y += 66;
        }
        list.pop_clip();
    }

    // ---- compose ---------------------------------------------------------------

    void draw_compose(gfx::DrawList &over) const
    {
        const auto &fonts = context_.fonts;
        const dc::Channel *open = open_channel();
        over.rounded_rect({0, 0, 1920, 1080}, 0, Color::rgb(0x000000, 0.85f));
        const std::string title =
            open ? (is_dm(*open) ? "@" + open->name : "#" + open->name) : std::string();
        label(over, fonts.semibold, title, 360, 432, 22, palette::interactive, 1200);
        const Rect box{360, 452, 1200, 96};
        over.rounded_rect(box, 12, palette::input);
        over.bordered_rect(box, 12, palette::clear, 2, palette::blurple);
        const std::string shown = draft_.empty() ? std::string(T("Mesajını yaz...")) : draft_;
        const float tail = fonts.regular.measure(shown, 28);
        const float scroll = std::max(0.0f, tail - (box.w - 64));
        over.push_clip({box.x + 16, box.y, box.w - 32, box.h});
        ui::text(over, fonts.regular, shown, box.x + 32 - scroll, box.y + 58, 28,
                 draft_.empty() ? palette::placeholder : palette::normal);
        if (std::fmod(clock_, 1.0f) < 0.55f)
        {
            const float caret_x = box.x + 32 - scroll + (draft_.empty() ? 0.0f : tail) + 2;
            over.rounded_rect({caret_x, box.y + 28, 2, 40}, 1, palette::normal);
        }
        over.pop_clip();
        ui::Canvas canvas{over, fonts, 0, clock_};
        keys_.draw(canvas);
    }

    app::Context &context_;
    dc::Snapshot snap_;
    float clock_ = 0.0f;
    std::string qr_url_;
    std::string invite_code_;
    std::vector<bool> invite_qr_;
    int invite_qr_size_ = 0;
    int invite_focus_ = 0; // 0: join, 1: later
    bool settings_open_ = false;
    int settings_section_ = 0;      // a Section
    bool settings_content_ = false; // the focus is in the section's rows, not the list
    int settings_row_ = 0;
    mutable std::string version_;
    mutable std::vector<bool> coffee_qr_;
    mutable int coffee_qr_size_ = 0;
    std::vector<bool> qr_;
    int qr_size_ = 0;
    Column column_ = Column::guilds;
    int guild_ = 0;
    int channel_ = 0;
    int post_ = 0; // the forum post the chat column has focused
    int scroll_ = 0;
    bool composing_ = false;
    bool confirm_logout_ = false;
    bool call_view_ = false; // the call screen replaces the chat while in a voice channel
    int logout_focus_ = 0;
    std::string draft_;
    ui::Keyboard keys_;
    Images images_;
    std::string rich_channel_;
    mutable std::unordered_map<std::string, RichText> rich_;
};

} // namespace

std::unique_ptr<app::Concept> make_discord(app::Context &context)
{
    return std::make_unique<Discord>(context);
}

} // namespace hui::concepts
