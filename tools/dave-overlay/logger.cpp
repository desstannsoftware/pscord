// ps5-discord - Stream-free stand-in for libdave's logger.cpp.
// Copyright (C) 2026 Desstann
// SPDX-License-Identifier: MIT

#include <dave/logger.h>

#include <atomic>

namespace discord {
namespace dave {

namespace {
std::atomic<LogSink> gLogSink = nullptr;
}

void SetLogSink(LogSink sink)
{
    gLogSink = sink;
}

LogStreamer::LogStreamer(LoggingSeverity severity, const char* file, int line)
  : severity_(severity)
  , file_(file)
  , line_(line)
{
}

LogStreamer::~LogStreamer()
{
    if (line_text_.empty() || severity_ == LS_NONE) {
        return;
    }
    if (auto sink = gLogSink.load()) {
        sink(severity_, file_, line_, line_text_);
    }
}

} // namespace dave
} // namespace discord
