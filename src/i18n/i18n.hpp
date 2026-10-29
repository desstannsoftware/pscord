// ps5-discord - The interface language: Turkish (the source text) or English.
// Copyright (C) 2026 Desstann
// SPDX-License-Identifier: MIT
//
// Text is written in Turkish and wrapped in T(); the Turkish text is the key
// for its English translation in i18n.cpp. Untranslated text stays Turkish.

#pragma once

#include <string>

namespace i18n {

enum class Lang { tr = 0, en = 1 };
constexpr int kLangCount = 2;

void set_language(Lang lang);
Lang language();
// The language's own name, as it is offered in the settings.
const char* language_name(Lang lang);

const char* T(const char* turkish);
// T(turkish) with its first "%s" replaced by arg.
std::string T(const char* turkish, const std::string& arg);

const char* month_name(int month);  // 0 = January
const char* weekday_name(int day);  // 0 = Sunday

} // namespace i18n
