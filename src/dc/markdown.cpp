// ps5-discord - Discord markdown: blocks line by line, then inline styles.
// Copyright (C) 2026 Desstann
// SPDX-License-Identifier: MIT

#include "dc/markdown.hpp"

#include "i18n/i18n.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace dc::md {

namespace {


// Decodes one UTF-8 code point at s[i]; advances i.
uint32_t next_cp(std::string_view s, size_t& i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    size_t n = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
    if (i + n > s.size()) {
        n = 1;
    }
    uint32_t cp = n == 1 ? c : n == 2 ? (c & 0x1f) : n == 3 ? (c & 0x0f) : (c & 0x07);
    for (size_t k = 1; k < n; ++k) {
        cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3f);
    }
    i += n;
    return cp;
}

bool regional(uint32_t cp) {
    return cp >= 0x1f1e6 && cp <= 0x1f1ff;
}

// Code points that start an emoji on their own.
bool emoji_base(uint32_t cp) {
    return (cp >= 0x1f000 && cp <= 0x1faff) || (cp >= 0x2600 && cp <= 0x27bf) || (cp >= 0x2300 && cp <= 0x23ff) ||
           (cp >= 0x2b00 && cp <= 0x2bff) || cp == 0x3030 || cp == 0x303d || cp == 0x3297 || cp == 0x3299 ||
           cp == 0x203c || cp == 0x2049;
}

// Code points that are emoji only when followed by U+FE0F (or a keycap).
bool emoji_text_default(uint32_t cp) {
    return cp == 0xa9 || cp == 0xae || cp == 0x2122 || cp == 0x2139 || (cp >= 0x2190 && cp <= 0x21ff) ||
           (cp >= 0x2934 && cp <= 0x2935) || cp == 0x24c2 || (cp >= 0x25aa && cp <= 0x25fe) || cp == '#' ||
           cp == '*' || (cp >= '0' && cp <= '9');
}

bool emoji_modifier(uint32_t cp) {
    return cp == 0xfe0f || cp == 0x20e3 || (cp >= 0x1f3fb && cp <= 0x1f3ff) || (cp >= 0xe0020 && cp <= 0xe007f);
}

bool digits(std::string_view s) {
    if (s.empty()) {
        return false;
    }
    for (char c : s) {
        if (c < '0' || c > '9') {
            return false;
        }
    }
    return true;
}

bool starts(std::string_view s, size_t i, std::string_view what) {
    return s.size() >= i + what.size() && s.compare(i, what.size(), what) == 0;
}

class Inline {
public:
    Inline(const Names& names, std::vector<Span>& out) : names_(names), out_(out) {}

    void run(std::string_view s, uint16_t style) {
        std::string buf;
        auto flush = [&] {
            if (!buf.empty()) {
                append_text(out_, buf, style, ++group_);
                buf.clear();
            }
        };
        size_t i = 0;
        while (i < s.size()) {
            const char c = s[i];
            if (c == '\\' && i + 1 < s.size() && std::strchr("\\*_~`|<>[]()#-:", s[i + 1])) {
                buf += s[i + 1];
                i += 2;
                continue;
            }
            if (c == '`') {
                size_t ticks = 0;
                while (i + ticks < s.size() && s[i + ticks] == '`' && ticks < 3) {
                    ++ticks;
                }
                const std::string fence(ticks, '`');
                const size_t end = s.find(fence, i + ticks);
                if (end != std::string_view::npos && end > i + ticks) {
                    flush();
                    std::string_view code = s.substr(i + ticks, end - i - ticks);
                    if (code.size() > 2 && code.front() == ' ' && code.back() == ' ') {
                        code = code.substr(1, code.size() - 2);
                    }
                    out_.push_back({std::string(code), uint16_t(style | kCode), {}, ++group_});
                    i = end + ticks;
                    continue;
                }
            }
            if (try_pair(s, i, "**", kBold, style, flush) || try_pair(s, i, "__", kUnderline, style, flush) ||
                try_pair(s, i, "~~", kStrike, style, flush) || try_pair(s, i, "||", kSpoiler, style, flush)) {
                continue;
            }
            if ((c == '*' || c == '_') && i + 1 < s.size() && s[i + 1] != ' ' &&
                (c == '*' || i == 0 || !std::isalnum(static_cast<unsigned char>(s[i - 1])))) {
                size_t end = i + 1;
                while ((end = s.find(c, end)) != std::string_view::npos) {
                    const bool doubled = end + 1 < s.size() && s[end + 1] == c;
                    if (s[end - 1] != ' ' && !doubled) {
                        break;
                    }
                    end += doubled ? 2 : 1;
                }
                if (end != std::string_view::npos && end > i + 1) {
                    flush();
                    run(s.substr(i + 1, end - i - 1), uint16_t(style | kItalic));
                    i = end + 1;
                    continue;
                }
            }
            if (c == '<') {
                const size_t end = s.find('>', i + 1);
                if (end != std::string_view::npos && tag(s.substr(i + 1, end - i - 1), style, flush)) {
                    i = end + 1;
                    continue;
                }
            }
            if (c == '[') {
                const size_t close = s.find("](", i + 1);
                const size_t paren = close == std::string_view::npos ? close : s.find(')', close + 2);
                if (paren != std::string_view::npos && s.find('\n', i) > paren &&
                    (starts(s, close + 2, "https://") || starts(s, close + 2, "http://"))) {
                    flush();
                    run(s.substr(i + 1, close - i - 1), uint16_t(style | kLink));
                    i = paren + 1;
                    continue;
                }
            }
            if ((c == 'h') && (starts(s, i, "https://") || starts(s, i, "http://"))) {
                size_t end = i;
                while (end < s.size() && s[end] != ' ' && s[end] != '\n' && s[end] != '<') {
                    ++end;
                }
                while (end > i && std::strchr(".,:;!?)\"'", s[end - 1])) {
                    --end;
                }
                flush();
                out_.push_back({std::string(s.substr(i, end - i)), uint16_t(style | kLink), {}, ++group_});
                i = end;
                continue;
            }
            buf += c;
            ++i;
        }
        flush();
    }

private:
    template <typename Flush>
    bool try_pair(std::string_view s, size_t& i, std::string_view mark, uint16_t add, uint16_t style, Flush& flush) {
        if (!starts(s, i, mark)) {
            return false;
        }
        const size_t end = s.find(mark, i + mark.size());
        if (end == std::string_view::npos || end == i + mark.size()) {
            return false;
        }
        flush();
        run(s.substr(i + mark.size(), end - i - mark.size()), uint16_t(style | add));
        i = end + mark.size();
        return true;
    }

    void mention(const std::string& text, uint16_t style) {
        out_.push_back({text, uint16_t(style | kMention), {}, ++group_});
    }

    // <@id>, <@!id>, <@&id>, <#id>, <:name:id>, <a:name:id>, <t:unix:f>, </cmd:id>, <https://...>
    template <typename Flush>
    bool tag(std::string_view inner, uint16_t style, Flush& flush) {
        if (inner.size() > 2 && inner[0] == '@' && inner[1] == '&' && digits(inner.substr(2))) {
            flush();
            const std::string name = names_.role ? names_.role(std::string(inner.substr(2))) : std::string();
            mention("@" + (name.empty() ? std::string("bilinmeyen-rol") : name), style);
            return true;
        }
        if (inner.size() > 1 && inner[0] == '@') {
            std::string_view id = inner.substr(inner.size() > 1 && inner[1] == '!' ? 2 : 1);
            if (digits(id)) {
                flush();
                const std::string name = names_.user ? names_.user(std::string(id)) : std::string();
                mention("@" + (name.empty() ? std::string(i18n::T("bilinmeyen-kullanıcı")) : name), style);
                return true;
            }
        }
        if (inner.size() > 1 && inner[0] == '#' && digits(inner.substr(1))) {
            flush();
            const std::string name = names_.channel ? names_.channel(std::string(inner.substr(1))) : std::string();
            mention("#" + (name.empty() ? std::string("bilinmeyen") : name), style);
            return true;
        }
        if (inner.size() > 3 && (inner[0] == ':' || (inner[0] == 'a' && inner[1] == ':'))) {
            const bool animated = inner[0] == 'a';
            std::string_view rest = inner.substr(animated ? 2 : 1);
            const size_t colon = rest.find(':');
            if (colon != std::string_view::npos && digits(rest.substr(colon + 1))) {
                flush();
                const std::string id(rest.substr(colon + 1));
                out_.push_back({":" + std::string(rest.substr(0, colon)) + ":", style, custom_emoji_url(id, animated),
                                ++group_});
                return true;
            }
        }
        if (inner.size() > 2 && inner[0] == 't' && inner[1] == ':') {
            std::string_view rest = inner.substr(2);
            const size_t colon = rest.find(':');
            std::string_view number = rest.substr(0, colon);
            if (digits(number)) {
                flush();
                const char kind = colon == std::string_view::npos || colon + 1 >= rest.size() ? 'f' : rest[colon + 1];
                const std::time_t when = std::time_t(std::strtoll(std::string(number).c_str(), nullptr, 10));
                out_.push_back({format_timestamp(when, kind, std::time(nullptr)), uint16_t(style | kTime), {}, ++group_});
                return true;
            }
        }
        if (inner.size() > 1 && inner[0] == '/') {
            const size_t colon = inner.find(':');
            if (colon != std::string_view::npos && digits(inner.substr(colon + 1))) {
                flush();
                mention(std::string(inner.substr(0, colon)), style);
                return true;
            }
        }
        if (starts(inner, 0, "https://") || starts(inner, 0, "http://")) {
            flush();
            out_.push_back({std::string(inner), uint16_t(style | kLink), {}, ++group_});
            return true;
        }
        return false;
    }

    const Names& names_;
    std::vector<Span>& out_;
    int group_ = 0;
};

std::string_view trim_left(std::string_view s, size_t& spaces) {
    spaces = 0;
    while (spaces < s.size() && s[spaces] == ' ') {
        ++spaces;
    }
    return s.substr(spaces);
}

} // namespace

std::string twemoji_url(const std::vector<uint32_t>& codepoints) {
    bool zwj = false;
    for (uint32_t cp : codepoints) {
        zwj = zwj || cp == 0x200d;
    }
    std::string name;
    for (uint32_t cp : codepoints) {
        if (cp == 0xfe0f && !zwj) {
            continue;
        }
        char hex[12];
        std::snprintf(hex, sizeof(hex), "%x", cp);
        name += (name.empty() ? "" : "-") + std::string(hex);
    }
    return "https://cdn.jsdelivr.net/gh/jdecked/twemoji@15.1.0/assets/72x72/" + name + ".png";
}

std::string custom_emoji_url(const std::string& id, bool animated) {
    return "https://cdn.discordapp.com/emojis/" + id + (animated ? ".gif" : ".png") + "?size=64";
}

std::string reaction_emoji_url(const std::string& name, const std::string& id, bool animated) {
    if (!id.empty()) {
        return custom_emoji_url(id, animated);
    }
    std::vector<uint32_t> cps;
    for (size_t i = 0; i < name.size();) {
        cps.push_back(next_cp(name, i));
    }
    return cps.empty() ? std::string() : twemoji_url(cps);
}

void append_text(std::vector<Span>& out, std::string_view text, uint16_t style, int group) {
    std::string plain;
    size_t i = 0;
    while (i < text.size()) {
        const size_t start = i;
        const uint32_t cp = next_cp(text, i);
        size_t peek = i;
        const uint32_t after = i < text.size() ? next_cp(text, peek) : 0;
        const bool starts_emoji = emoji_base(cp) || (regional(cp) && regional(after)) ||
                                  (emoji_text_default(cp) && (after == 0xfe0f || after == 0x20e3));
        if (!starts_emoji) {
            plain.append(text.substr(start, i - start));
            continue;
        }
        std::vector<uint32_t> cps{cp};
        if (regional(cp)) {
            cps.push_back(after);
            i = peek;
        } else {
            for (;;) {
                if (i >= text.size()) {
                    break;
                }
                size_t j = i;
                const uint32_t next = next_cp(text, j);
                if (emoji_modifier(next)) {
                    cps.push_back(next);
                    i = j;
                } else if (next == 0x200d && j < text.size()) {
                    size_t k = j;
                    const uint32_t joined = next_cp(text, k);
                    cps.push_back(next);
                    cps.push_back(joined);
                    i = k;
                } else {
                    break;
                }
            }
        }
        if (!plain.empty()) {
            out.push_back({plain, style, {}, group});
            plain.clear();
        }
        out.push_back({std::string(text.substr(start, i - start)), style, twemoji_url(cps), group});
    }
    if (!plain.empty()) {
        out.push_back({plain, style, {}, group});
    }
}

std::vector<Block> parse(const std::string& text, const Names& names) {
    std::vector<Block> blocks;
    std::vector<std::string_view> lines;
    std::string_view all(text);
    for (size_t start = 0;;) {
        const size_t nl = all.find('\n', start);
        lines.push_back(all.substr(start, nl == std::string_view::npos ? std::string_view::npos : nl - start));
        if (nl == std::string_view::npos) {
            break;
        }
        start = nl + 1;
    }
    bool quote_rest = false;
    for (size_t li = 0; li < lines.size(); ++li) {
        std::string_view line = lines[li];
        Block block;
        if (quote_rest) {
            block.quote = true;
        } else if (starts(line, 0, ">>> ")) {
            quote_rest = true;
            block.quote = true;
            line = line.substr(4);
        } else if (starts(line, 0, "> ") || line == ">") {
            block.quote = true;
            line = line.substr(line.size() > 1 ? 2 : 1);
        }

        size_t spaces = 0;
        std::string_view body = trim_left(line, spaces);
        if (starts(body, 0, "```")) {
            std::string code;
            std::string_view rest = body.substr(3);
            const size_t close_same = rest.find("```");
            if (close_same != std::string_view::npos) {
                code = std::string(rest.substr(0, close_same));
            } else {
                // The opening line may name a language; the code starts on the next one.
                bool first = true;
                for (++li; li < lines.size(); ++li) {
                    std::string_view l = lines[li];
                    const size_t end = l.find("```");
                    std::string_view part = end == std::string_view::npos ? l : l.substr(0, end);
                    if (!first || !part.empty() || end == std::string_view::npos) {
                        code += (first ? "" : "\n") + std::string(part);
                        first = false;
                    }
                    if (end != std::string_view::npos) {
                        break;
                    }
                }
            }
            while (!code.empty() && code.back() == '\n') {
                code.pop_back();
            }
            block.kind = Kind::code_block;
            block.spans.push_back({code, kCode, {}, 1});
            blocks.push_back(std::move(block));
            continue;
        }
        if (starts(body, 0, "# ")) {
            block.kind = Kind::h1;
            body = body.substr(2);
        } else if (starts(body, 0, "## ")) {
            block.kind = Kind::h2;
            body = body.substr(3);
        } else if (starts(body, 0, "### ")) {
            block.kind = Kind::h3;
            body = body.substr(4);
        } else if (starts(body, 0, "-# ")) {
            block.kind = Kind::subtext;
            body = body.substr(3);
        } else if (starts(body, 0, "- ") || starts(body, 0, "* ")) {
            block.list = int(spaces / 2) + 1;
            block.bullet = block.list > 1 ? "◦" : "•";
            body = body.substr(2);
        } else {
            size_t n = 0;
            while (n < body.size() && n < 4 && body[n] >= '0' && body[n] <= '9') {
                ++n;
            }
            if (n > 0 && starts(body, n, ". ")) {
                block.list = int(spaces / 2) + 1;
                block.bullet = std::string(body.substr(0, n + 1));
                body = body.substr(n + 2);
            } else {
                body = line;
            }
        }
        Inline(names, block.spans).run(body, block.kind == Kind::h1 || block.kind == Kind::h2 ||
                                                     block.kind == Kind::h3
                                                 ? kBold
                                                 : 0);
        blocks.push_back(std::move(block));
    }
    return blocks;
}

bool emoji_only(const std::vector<Block>& blocks) {
    int count = 0;
    for (const Block& b : blocks) {
        if (b.kind != Kind::paragraph || b.quote || b.list) {
            return false;
        }
        for (const Span& s : b.spans) {
            if (!s.emoji_url.empty()) {
                ++count;
            } else if (s.text.find_first_not_of(" \t") != std::string::npos) {
                return false;
            }
        }
    }
    return count > 0 && count <= 30;
}

std::string format_timestamp(std::time_t when, char style, std::time_t now) {
    std::tm t{};
    localtime_r(&when, &t);
    char buf[96];
    switch (style) {
    case 't':
        std::snprintf(buf, sizeof(buf), "%02d:%02d", t.tm_hour, t.tm_min);
        break;
    case 'T':
        std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
        break;
    case 'd':
        std::snprintf(buf, sizeof(buf), "%02d.%02d.%04d", t.tm_mday, t.tm_mon + 1, t.tm_year + 1900);
        break;
    case 'D':
        std::snprintf(buf, sizeof(buf), "%d %s %d", t.tm_mday, i18n::month_name(t.tm_mon), t.tm_year + 1900);
        break;
    case 'F':
        std::snprintf(buf, sizeof(buf), "%d %s %d %s %02d:%02d", t.tm_mday, i18n::month_name(t.tm_mon), t.tm_year + 1900,
                      i18n::weekday_name(t.tm_wday), t.tm_hour, t.tm_min);
        break;
    case 'R': {
        const long long diff = (long long)(now - when);
        const long long a = diff < 0 ? -diff : diff;
        const bool english = i18n::language() == i18n::Lang::en;
        const char* unit;
        long long n;
        if (a < 60) {
            n = a, unit = english ? "second" : "saniye";
        } else if (a < 3600) {
            n = a / 60, unit = english ? "minute" : "dakika";
        } else if (a < 86400) {
            n = a / 3600, unit = english ? "hour" : "saat";
        } else if (a < 86400 * 30) {
            n = a / 86400, unit = english ? "day" : "gün";
        } else if (a < 86400 * 365) {
            n = a / (86400 * 30), unit = english ? "month" : "ay";
        } else {
            n = a / (86400 * 365), unit = english ? "year" : "yıl";
        }
        if (english) {
            const char* plural = n == 1 ? "" : "s";
            std::snprintf(buf, sizeof(buf), diff >= 0 ? "%lld %s%s ago" : "in %lld %s%s", n, unit, plural);
        } else {
            std::snprintf(buf, sizeof(buf), diff >= 0 ? "%lld %s önce" : "%lld %s içinde", n, unit);
        }
        break;
    }
    default:
        std::snprintf(buf, sizeof(buf), "%d %s %d %02d:%02d", t.tm_mday, i18n::month_name(t.tm_mon), t.tm_year + 1900,
                      t.tm_hour, t.tm_min);
        break;
    }
    return buf;
}

} // namespace dc::md
