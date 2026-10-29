// ps5-discord - Discord's flavour of markdown, parsed into styled spans.
// Copyright (C) 2026 Desstann
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <ctime>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace dc::md {

enum Style : uint16_t {
    kBold = 1,
    kItalic = 2,
    kUnderline = 4,
    kStrike = 8,
    kCode = 16,
    kLink = 32,
    kMention = 64,
    kSpoiler = 128,
    kTime = 256,
};

struct Span {
    std::string text;      // for an emoji: the character itself, or :name:
    uint16_t style = 0;
    std::string emoji_url; // set for emoji, Unicode or custom
    int group = 0;         // spans cut from one token share it (a mention, a code run)
};

enum class Kind { paragraph, h1, h2, h3, subtext, code_block };

struct Block {
    Kind kind = Kind::paragraph;
    bool quote = false;
    int list = 0;       // nesting level of a list item, 0 for none
    std::string bullet; // "•" or "3."
    std::vector<Span> spans;
};

struct Names {
    std::function<std::string(const std::string&)> user;
    std::function<std::string(const std::string&)> channel;
    std::function<std::string(const std::string&)> role;
};

std::vector<Block> parse(const std::string& text, const Names& names);

// True when the message is only emoji (at most 30): Discord draws them large.
bool emoji_only(const std::vector<Block>& blocks);

// Plain text cut into text and emoji spans.
void append_text(std::vector<Span>& out, std::string_view text, uint16_t style, int group);

std::string twemoji_url(const std::vector<uint32_t>& codepoints);
std::string custom_emoji_url(const std::string& id, bool animated);
// A Unicode emoji (as text) or name:id of a custom one, as a reaction carries it.
std::string reaction_emoji_url(const std::string& name, const std::string& id, bool animated);

// Discord's <t:unix:style> in Turkish.
std::string format_timestamp(std::time_t when, char style, std::time_t now);

} // namespace dc::md
