// ps5-discord - Stream-free stand-in for libdave's logger.h.
// Copyright (C) 2026 Desstann
// SPDX-License-Identifier: MIT
//
// Same interface as upstream, but formats into a std::string: the console's
// libc has no locale support, which libc++'s ostream needs.
#pragma once

#include <cstdio>
#include <string>
#include <string_view>
#include <type_traits>

#include <dave/dave_interfaces.h>

#if !defined(DISCORD_LOG)
#define DISCORD_LOG_FILE_LINE(sev, file, line) ::discord::dave::LogStreamer(sev, file, line)
#define DISCORD_LOG(sev) DISCORD_LOG_FILE_LINE(::discord::dave::sev, __FILE__, __LINE__)
#endif

namespace discord {
namespace dave {

using LogSink = void (*)(LoggingSeverity severity,
                         const char* file,
                         int line,
                         const std::string& message);
void SetLogSink(LogSink sink);

class LogStreamer {
public:
    LogStreamer(LoggingSeverity severity, const char* file, int line);
    ~LogStreamer();

    template <typename T>
    LogStreamer& operator<<(const T& value)
    {
        if constexpr (std::is_convertible_v<const T&, std::string_view>) {
            line_text_ += std::string_view(value);
        }
        else if constexpr (std::is_same_v<T, bool>) {
            line_text_ += value ? "true" : "false";
        }
        else if constexpr (std::is_same_v<T, char>) {
            line_text_ += value;
        }
        else if constexpr (std::is_enum_v<T>) {
            line_text_ += std::to_string(static_cast<long long>(value));
        }
        else if constexpr (std::is_arithmetic_v<T>) {
            line_text_ += std::to_string(value);
        }
        else if constexpr (std::is_pointer_v<T>) {
            char text[24];
            std::snprintf(text, sizeof(text), "%p", static_cast<const void*>(value));
            line_text_ += text;
        }
        else {
            line_text_ += "<?>";
        }
        return *this;
    }

private:
    LoggingSeverity severity_;
    const char* file_;
    int line_;
    std::string line_text_;
};

} // namespace dave
} // namespace discord
