// PS5 Discord - Network pictures, rich text layout and channel icons.
// Copyright (C) 2026 Desstann
// SPDX-License-Identifier: GPL-3.0-or-later

#include "concepts/discord_ui.hpp"

#include "dc/media.hpp"
#include "ui/widgets.hpp"

#include <GL/glcorearb.h>

#include <algorithm>
#include <cmath>

namespace hui::concepts::discord
{

namespace
{

// GPU memory the pictures may hold before old ones are dropped.
constexpr std::size_t kBudget = 320u * 1024 * 1024;
constexpr int kUploadsPerFrame = 6;

std::uint32_t upload(int w, int h, const std::uint8_t *rgba)
{
    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    // One level only: mip chains leave ps5-opengl's batched fast path.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return texture;
}

const ui::FontRef &font_of(const ui::Fonts &fonts, int font)
{
    return font == 1 ? fonts.semibold : font == 2 ? fonts.mono : fonts.regular;
}

} // namespace

// ---- Images -----------------------------------------------------------------

Images::~Images()
{
    for (auto &[url, entry] : entries_)
        if (!entry.frames.empty())
            glDeleteTextures(static_cast<GLsizei>(entry.frames.size()), entry.frames.data());
}

std::uint32_t Images::get(const std::string &url, int max_w, int max_h, float clock) const
{
    if (url.empty())
        return 0;
    auto it = entries_.find(url);
    if (it == entries_.end())
    {
        entries_.emplace(url, Entry{});
        dc::Media::instance().request(url, max_w, max_h);
        return 0;
    }
    Entry &entry = it->second;
    entry.used = tick_;
    if (entry.frames.empty())
        return 0;
    if (entry.frames.size() == 1 || entry.total_ms <= 0)
        return entry.frames[0];
    int t = static_cast<int>(std::fmod(clock * 1000.0f, static_cast<float>(entry.total_ms)));
    for (std::size_t i = 0; i < entry.frames.size(); ++i)
    {
        t -= entry.delays_ms[i];
        if (t < 0)
            return entry.frames[i];
    }
    return entry.frames.back();
}

void Images::update()
{
    ++tick_;
    dc::Picture picture;
    for (int i = 0; i < kUploadsPerFrame && dc::Media::instance().take(picture); ++i)
    {
        Entry &entry = entries_[picture.url];
        entry.used = tick_;
        if (picture.width == 0 || picture.frames.empty())
        {
            entry.failed = true;
            continue;
        }
        for (const auto &frame : picture.frames)
            entry.frames.push_back(upload(picture.width, picture.height, frame.data()));
        entry.delays_ms = picture.delays_ms;
        entry.total_ms = 0;
        for (int d : entry.delays_ms)
            entry.total_ms += d;
        entry.bytes = static_cast<std::size_t>(picture.width) *
                      static_cast<std::size_t>(picture.height) * 4 * picture.frames.size();
        bytes_ += entry.bytes;
    }
    if (bytes_ > kBudget)
        evict();
}

void Images::evict()
{
    // Oldest first, never what was drawn in the last few seconds.
    std::vector<std::pair<std::uint64_t, std::string>> order;
    for (const auto &[url, entry] : entries_)
        if (!entry.frames.empty() && tick_ - entry.used > 180)
            order.emplace_back(entry.used, url);
    std::sort(order.begin(), order.end());
    for (const auto &[used, url] : order)
    {
        if (bytes_ <= kBudget * 3 / 4)
            break;
        Entry &entry = entries_[url];
        glDeleteTextures(static_cast<GLsizei>(entry.frames.size()), entry.frames.data());
        bytes_ -= entry.bytes;
        dc::Media::instance().forget(url);
        entries_.erase(url);
    }
}

// ---- rich text --------------------------------------------------------------

namespace
{

struct Piece
{
    std::string text;
    std::string emoji;
    int font = 0;
    float size = 24;
    float w = 0;
    gfx::Color color;
    std::uint16_t style = 0;
    int group = 0;
    bool space = false;
};

struct LineBuilder
{
    const ui::Fonts &fonts;
    RichText &out;
    float width;
    float x0;
    float line_h;
    float emoji_size;
    float y;
    float x;
    std::vector<Piece> line;

    void flush()
    {
        while (!line.empty() && line.back().space)
            line.pop_back();
        float h = line_h;
        for (const Piece &p : line)
            if (!p.emoji.empty())
                h = std::max(h, p.w + 6);
        const float mid = y + h * 0.5f;
        float cx = x0;
        int box_group = -1;
        std::uint16_t box_kind = 0;
        for (const Piece &p : line)
        {
            RichRun run;
            run.x = cx;
            run.w = p.w;
            run.text = p.text;
            run.emoji = p.emoji;
            run.font = p.font;
            run.size = p.size;
            run.color = p.color;
            run.style = p.style;
            run.baseline = mid + p.size * 0.36f;
            run.top = mid - p.w * 0.5f;
            const std::uint16_t kind =
                p.style & (dc::md::kCode | dc::md::kMention | dc::md::kTime | dc::md::kSpoiler);
            if (kind)
            {
                const float bh = p.size * 1.3f;
                if (kind == box_kind && p.group == box_group && !out.under.empty())
                    out.under.back().r.w = cx + p.w + 3 - out.under.back().r.x;
                else
                {
                    RichBox box;
                    box.r = {cx - 3, mid - bh * 0.5f, p.w + 6, bh};
                    box.radius = 4;
                    if (kind & dc::md::kCode)
                    {
                        box.fill = palette::code_bg;
                        box.border = palette::code_border;
                    }
                    else if (kind & dc::md::kMention)
                        box.fill = palette::mention_bg;
                    else if (kind & dc::md::kSpoiler)
                        box.fill = gfx::Color::rgb(0xffffff, 0.1f);
                    else
                        box.fill = gfx::Color::rgb(0xffffff, 0.06f);
                    out.under.push_back(box);
                    box_group = p.group;
                    box_kind = kind;
                }
            }
            else
            {
                box_group = -1;
                box_kind = 0;
            }
            if (p.style & dc::md::kStrike)
                out.over.push_back({{cx, mid - 1, p.w, 2}, 0, p.color});
            if (p.style & (dc::md::kUnderline))
                out.over.push_back({{cx, run.baseline + 3, p.w, 2}, 0, p.color});
            if (!p.space || !run.text.empty())
                out.runs.push_back(run);
            cx += p.w;
        }
        out.width = std::max(out.width, cx);
        line.clear();
        y += h;
        x = x0;
    }

    void add(Piece p)
    {
        if (p.space && line.empty())
            return;
        if (!p.space && x + p.w > width && !line.empty())
            flush();
        if (!p.space && p.w > width - x0 && p.emoji.empty())
        {
            // One word wider than the line: cut it where it overflows.
            const ui::FontRef &font = font_of(fonts, p.font);
            std::string rest = p.text;
            while (!rest.empty())
            {
                std::size_t take = 0;
                std::size_t i = 0;
                while (i < rest.size())
                {
                    std::size_t n = 1;
                    const unsigned char c = static_cast<unsigned char>(rest[i]);
                    n = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
                    if (font.measure(rest.substr(0, i + n), p.size) > width - x && take > 0)
                        break;
                    i += n;
                    take = i;
                }
                Piece part = p;
                part.text = rest.substr(0, take);
                part.w = font.measure(part.text, p.size);
                line.push_back(part);
                x += part.w;
                rest.erase(0, take);
                if (!rest.empty())
                    flush();
            }
            return;
        }
        line.push_back(p);
        x += p.w;
    }
};

} // namespace

RichText layout_rich(const std::vector<dc::md::Block> &blocks, const ui::Fonts &fonts, float width,
                     float base, gfx::Color color)
{
    RichText out;
    const bool jumbo = dc::md::emoji_only(blocks);
    float y = 0;
    for (std::size_t bi = 0; bi < blocks.size(); ++bi)
    {
        const dc::md::Block &b = blocks[bi];
        float size = base;
        int font = 0;
        gfx::Color text_color = color;
        float gap = 0;
        switch (b.kind)
        {
        case dc::md::Kind::h1:
            size = base * 1.5f;
            font = 1;
            gap = bi ? 12.0f : 0.0f;
            break;
        case dc::md::Kind::h2:
            size = base * 1.25f;
            font = 1;
            gap = bi ? 12.0f : 0.0f;
            break;
        case dc::md::Kind::h3:
            font = 1;
            gap = bi ? 8.0f : 0.0f;
            break;
        case dc::md::Kind::subtext:
            size = base * 0.8125f;
            text_color = palette::muted;
            break;
        default:
            break;
        }
        y += gap;
        const float line_h = std::round(size * 1.375f);
        float x0 = 0;
        const float block_top = y;
        if (b.quote)
            x0 += 24;
        if (b.list)
        {
            x0 += static_cast<float>(b.list - 1) * 30 + 36;
        }

        if (b.kind == dc::md::Kind::code_block)
        {
            const float code_size = std::round(base * 0.875f);
            const float code_h = std::round(code_size * 1.35f);
            const float pad = 12;
            std::vector<std::string> lines;
            const std::string &code = b.spans.empty() ? std::string() : b.spans[0].text;
            for (std::size_t start = 0;;)
            {
                const std::size_t nl = code.find('\n', start);
                std::string line =
                    code.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
                for (const std::string &part : fonts.mono.font->wrap(
                         line.empty() ? " " : line, code_size, width - x0 - pad * 2))
                    lines.push_back(part);
                if (nl == std::string::npos)
                    break;
                start = nl + 1;
            }
            y += 6;
            const float h = static_cast<float>(lines.size()) * code_h + pad * 2;
            out.under.push_back(
                {{x0, y, width - x0, h}, 6, palette::code_bg, palette::code_border});
            float line_y = y + pad;
            for (const std::string &line : lines)
            {
                RichRun run;
                run.x = x0 + pad;
                run.baseline = line_y + code_h * 0.5f + code_size * 0.36f;
                run.text = line;
                run.font = 2;
                run.size = code_size;
                run.color = palette::normal;
                out.runs.push_back(run);
                line_y += code_h;
            }
            y += h + 6;
            out.width = width;
        }
        else
        {
            LineBuilder builder{fonts, out,    width,
                                x0,    line_h, jumbo ? 72.0f : std::round(size * 1.375f),
                                y,     x0,     {}};
            if (b.list)
            {
                RichRun bullet;
                bullet.x = x0 - 24;
                bullet.baseline = y + line_h * 0.5f + size * 0.36f;
                bullet.text = b.bullet;
                bullet.size = size;
                bullet.color = text_color;
                if (b.bullet.size() > 1 && b.bullet.back() == '.')
                    bullet.x = x0 - 12 - fonts.regular.measure(b.bullet, size);
                out.runs.push_back(bullet);
            }
            for (const dc::md::Span &span : b.spans)
            {
                Piece base_piece;
                base_piece.style = span.style;
                base_piece.group = span.group;
                base_piece.font = (span.style & dc::md::kCode)           ? 2
                                  : (span.style & dc::md::kBold) || font ? 1
                                                                         : 0;
                base_piece.size = (span.style & dc::md::kCode) ? std::round(size * 0.875f) : size;
                base_piece.color = (span.style & dc::md::kLink)      ? palette::link
                                   : (span.style & dc::md::kMention) ? palette::mention_text
                                   : (span.style & kEdited)          ? palette::muted
                                                                     : text_color;
                if (span.style & kEdited)
                    base_piece.size = std::round(size * 0.625f);
                if (!span.emoji_url.empty())
                {
                    Piece p = base_piece;
                    p.emoji = span.emoji_url;
                    p.w = builder.emoji_size;
                    builder.add(p);
                    continue;
                }
                const ui::FontRef &f = font_of(fonts, base_piece.font);
                const std::string &t = span.text;
                std::size_t i = 0;
                while (i < t.size())
                {
                    const bool space = t[i] == ' ';
                    std::size_t j = i;
                    while (j < t.size() && (t[j] == ' ') == space)
                        ++j;
                    Piece p = base_piece;
                    p.text = t.substr(i, j - i);
                    p.space = space && !(span.style & (dc::md::kCode | dc::md::kMention));
                    p.w = f.measure(p.text, p.size);
                    builder.add(p);
                    i = j;
                }
            }
            if (!builder.line.empty() || b.spans.empty())
            {
                if (builder.line.empty())
                    builder.y += line_h;
                else
                    builder.flush();
            }
            y = builder.y;
        }
        if (b.quote)
            out.under.push_back({{0, block_top, 6, y - block_top}, 3, palette::dim});
    }
    out.height = y;
    return out;
}

void draw_rich(gfx::DrawList &list, const ui::Fonts &fonts, const RichText &text, float x, float y,
               const Images &images, float clock)
{
    for (const RichBox &b : text.under)
    {
        const gfx::Rect r{x + b.r.x, y + b.r.y, b.r.w, b.r.h};
        if (b.border.a > 0.0f)
            list.bordered_rect(r, b.radius, b.fill, 1.5f, b.border);
        else
            list.rounded_rect(r, b.radius, b.fill);
    }
    for (const RichRun &run : text.runs)
    {
        if (!run.emoji.empty())
        {
            const int px = run.w > 40 ? 96 : 48;
            if (const std::uint32_t tex = images.get(run.emoji, px, px, clock))
                list.image(tex, {x + run.x, y + run.top, run.w, run.w}, gfx::kFullUv,
                           palette::white);
            continue;
        }
        ui::text(list, font_of(fonts, run.font), run.text, x + run.x, y + run.baseline, run.size,
                 run.color);
    }
    for (const RichBox &b : text.over)
        list.rounded_rect({x + b.r.x, y + b.r.y, b.r.w, b.r.h}, b.radius, b.fill);
}

float measure_label(const ui::FontRef &font, const std::string &text, float size)
{
    std::vector<dc::md::Span> spans;
    dc::md::append_text(spans, text, 0, 0);
    float w = 0;
    for (const auto &s : spans)
        w += s.emoji_url.empty() ? font.measure(s.text, size) : std::round(size * 1.25f) + 2;
    return w;
}

float draw_label(gfx::DrawList &list, const ui::Fonts &fonts, const ui::FontRef &font,
                 const std::string &text, float x, float baseline, float size, gfx::Color color,
                 float max_w, const Images &images, float clock)
{
    (void)fonts;
    std::vector<dc::md::Span> spans;
    dc::md::append_text(spans, text, 0, 0);
    const float emoji = std::round(size * 1.25f);
    const float dots = font.measure("...", size);
    float cx = x;
    const bool cut = measure_label(font, text, size) > max_w;
    const float limit = cut ? x + max_w - dots : x + max_w;
    for (const auto &s : spans)
    {
        if (!s.emoji_url.empty())
        {
            if (cx + emoji > limit)
                break;
            if (const std::uint32_t tex = images.get(s.emoji_url, 48, 48, clock))
                list.image(tex, {cx, baseline - size * 0.36f - emoji * 0.5f, emoji, emoji},
                           gfx::kFullUv,
                           color.a < 1.0f ? palette::white.with_alpha(color.a) : palette::white);
            cx += emoji + 2;
            continue;
        }
        std::string part = s.text;
        if (cx + font.measure(part, size) > limit)
        {
            while (!part.empty() && cx + font.measure(part, size) > limit)
            {
                while (!part.empty() && (static_cast<unsigned char>(part.back()) & 0xc0) == 0x80)
                    part.pop_back();
                if (!part.empty())
                    part.pop_back();
            }
            ui::text(list, font, part, cx, baseline, size, color);
            cx += font.measure(part, size);
            break;
        }
        ui::text(list, font, part, cx, baseline, size, color);
        cx += font.measure(part, size);
    }
    if (cut)
    {
        ui::text(list, font, "...", cx, baseline, size, color);
        cx += dots;
    }
    return cx - x;
}

// ---- channel icons ----------------------------------------------------------

void draw_channel_icon(gfx::DrawList &list, int type, bool rules, float x, float cy, float size,
                       gfx::Color color)
{
    const float s = size / 24.0f; // drawn on a 24-unit grid, as Discord's icons are
    const float t = 2.2f * s;
    auto L = [&](float x1, float y1, float x2, float y2)
    { list.line(x + x1 * s, cy + (y1 - 12) * s, x + x2 * s, cy + (y2 - 12) * s, t, color); };
    if (rules)
    {
        // A book with a tick.
        list.bordered_rect({x + 4 * s, cy - 9 * s, 16 * s, 18 * s}, 2.5f * s, palette::clear, t,
                           color);
        L(8, 12, 11, 15);
        L(11, 15, 16, 9);
        return;
    }
    switch (type)
    {
    case 2: // voice: speaker and two waves
    case 13:
    {
        const float poly[] = {x + 3 * s,  cy - 3.5f * s, x + 7 * s,  cy - 3.5f * s,
                              x + 12 * s, cy - 8 * s,    x + 12 * s, cy + 8 * s,
                              x + 7 * s,  cy + 3.5f * s, x + 3 * s,  cy + 3.5f * s};
        list.polygon(poly, 6, color);
        L(15, 9, 16, 12);
        L(16, 12, 15, 15);
        L(18, 6, 20, 12);
        L(20, 12, 18, 18);
        if (type == 13)
            list.ring(x + 12 * s, cy, 11 * s, t * 0.6f, color);
        return;
    }
    case 5:
    { // announcement: a megaphone
        const float poly[] = {x + 3 * s,  cy - 4 * s, x + 8 * s,  cy - 4 * s,
                              x + 18 * s, cy - 9 * s, x + 18 * s, cy + 9 * s,
                              x + 8 * s,  cy + 4 * s, x + 3 * s,  cy + 4 * s};
        list.polygon(poly, 6, color);
        L(7, 16, 9, 21);
        L(20, 10, 21, 10);
        L(20, 14, 21, 14);
        return;
    }
    case 15: // forum: two speech bubbles
        list.bordered_rect({x + 2 * s, cy - 9 * s, 14 * s, 11 * s}, 3 * s, palette::clear, t,
                           color);
        list.bordered_rect({x + 8 * s, cy - 1 * s, 14 * s, 10 * s}, 3 * s, palette::clear, t,
                           color);
        return;
    case 16: // media: a picture frame
        list.bordered_rect({x + 2 * s, cy - 8 * s, 20 * s, 16 * s}, 3 * s, palette::clear, t,
                           color);
        list.circle(x + 8 * s, cy - 3 * s, 2 * s, color);
        L(5, 18, 11, 13);
        L(11, 13, 19, 19);
        return;
    default: // text: Discord's slanted hash
        L(9, 4, 7, 20);
        L(17, 4, 15, 20);
        L(4, 9, 20, 9);
        L(3, 15, 19, 15);
        return;
    }
}

} // namespace hui::concepts::discord
