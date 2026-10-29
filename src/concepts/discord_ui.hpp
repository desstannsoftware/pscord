// PS5 Discord - Pieces of the Discord screen: palette, network pictures as
// textures, rich text (markdown, emoji, mentions) and channel icons.
// Copyright (C) 2026 Desstann
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "dc/markdown.hpp"
#include "gfx/draw_list.hpp"
#include "ui/fonts.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace hui::concepts::discord
{

// Discord's dark theme.
namespace palette
{
inline const gfx::Color blurple = gfx::Color::rgb(0x5865f2);
inline const gfx::Color blurple_deep = gfx::Color::rgb(0x3c45a5);
inline const gfx::Color rail = gfx::Color::rgb(0x1e1f22);
inline const gfx::Color sidebar = gfx::Color::rgb(0x2b2d31);
inline const gfx::Color chat = gfx::Color::rgb(0x313338);
inline const gfx::Color panel = gfx::Color::rgb(0x232428);
inline const gfx::Color input = gfx::Color::rgb(0x383a40);
inline const gfx::Color hover = gfx::Color::rgb(0x35373c);
inline const gfx::Color selected = gfx::Color::rgb(0x404249);
inline const gfx::Color message_hover = gfx::Color::rgb(0x2e3035);
inline const gfx::Color divider = gfx::Color::rgb(0x1f2023);
inline const gfx::Color separator = gfx::Color::rgb(0x3f4147);
inline const gfx::Color header = gfx::Color::rgb(0xf2f3f5);
inline const gfx::Color normal = gfx::Color::rgb(0xdbdee1);
inline const gfx::Color muted = gfx::Color::rgb(0x949ba4);
inline const gfx::Color faint = gfx::Color::rgb(0x80848e);
inline const gfx::Color dim = gfx::Color::rgb(0x4e5058);
inline const gfx::Color placeholder = gfx::Color::rgb(0x6d6f78);
inline const gfx::Color interactive = gfx::Color::rgb(0xb5bac1);
inline const gfx::Color link = gfx::Color::rgb(0x00a8fc);
inline const gfx::Color mention_text = gfx::Color::rgb(0xc9cdfb);
inline const gfx::Color mention_bg = gfx::Color::rgb(0x5865f2, 0.3f);
inline const gfx::Color code_bg = gfx::Color::rgb(0x2b2d31);
inline const gfx::Color code_border = gfx::Color::rgb(0x1e1f22);
inline const gfx::Color green = gfx::Color::rgb(0x23a55a);
inline const gfx::Color idle = gfx::Color::rgb(0xf0b232);
inline const gfx::Color red = gfx::Color::rgb(0xf23f43);
inline const gfx::Color white = gfx::Color::rgb(0xffffff);
inline const gfx::Color clear = gfx::Color::rgb(0x000000, 0.0f);
} // namespace palette

// Discord's role colours are 0xRRGGBB, 0 meaning "none".
inline gfx::Color role_color(std::uint32_t color, gfx::Color fallback)
{
    return color ? gfx::Color::rgb(color) : fallback;
}

// Network pictures as GL textures: asked for while drawing, uploaded in update().
class Images
{
  public:
    Images() = default;
    Images(const Images &) = delete;
    Images &operator=(const Images &) = delete;
    ~Images();

    // The texture for url (the frame due at `clock` for a GIF), or 0 while it loads.
    std::uint32_t get(const std::string &url, int max_w, int max_h, float clock) const;
    void update();

  private:
    struct Entry
    {
        std::vector<std::uint32_t> frames;
        std::vector<int> delays_ms;
        int total_ms = 0;
        std::size_t bytes = 0;
        bool failed = false;
        std::uint64_t used = 0;
    };
    void evict();

    mutable std::unordered_map<std::string, Entry> entries_;
    mutable std::uint64_t tick_ = 0;
    std::size_t bytes_ = 0;
};

// Extra style bit for "(düzenlendi)" after an edited message.
constexpr std::uint16_t kEdited = 0x200;

struct RichRun
{
    float x = 0;
    float baseline = 0;
    float top = 0; // of an emoji
    float w = 0;
    std::string text;
    std::string emoji;
    int font = 0; // 0 regular, 1 semibold, 2 mono
    float size = 24;
    gfx::Color color;
    std::uint16_t style = 0;
};

struct RichBox
{
    gfx::Rect r;
    float radius = 0;
    gfx::Color fill;
    gfx::Color border = palette::clear;
};

struct RichText
{
    std::vector<RichBox> under;
    std::vector<RichRun> runs;
    std::vector<RichBox> over;
    float height = 0;
    float width = 0;
};

// Lays blocks out in `width` at a base text size (24 for message text).
RichText layout_rich(const std::vector<dc::md::Block> &blocks, const ui::Fonts &fonts, float width,
                     float base, gfx::Color color);
void draw_rich(gfx::DrawList &list, const ui::Fonts &fonts, const RichText &text, float x, float y,
               const Images &images, float clock);

// One line with emoji drawn as pictures, cut with "..." past max_w. Returns its width.
float draw_label(gfx::DrawList &list, const ui::Fonts &fonts, const ui::FontRef &font,
                 const std::string &text, float x, float baseline, float size, gfx::Color color,
                 float max_w, const Images &images, float clock);
float measure_label(const ui::FontRef &font, const std::string &text, float size);

// The glyph in front of a channel name: #, megaphone, speaker, rules book...
void draw_channel_icon(gfx::DrawList &list, int type, bool rules, float x, float cy, float size,
                       gfx::Color color);

} // namespace hui::concepts::discord
