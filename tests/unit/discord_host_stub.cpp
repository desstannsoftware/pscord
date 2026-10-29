// PSCord - Host-only stand-in for the Discord screen in GoogleTest.
// Copyright (C) 2026 Desstann
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The real Discord concept pulls the network client, voice, updater and
// OpenGL texture uploads. Unit tests only need a registry entry that draws
// and survives input; the console build links the real src/concepts/discord*.

#include "concepts/concepts.hpp"

#include "gfx/draw_list.hpp"

namespace hui::concepts
{
namespace
{

class DiscordHostStub final : public app::Concept
{
  public:
    explicit DiscordHostStub(app::Context &)
    {
    }

    const app::ConceptInfo &info() const override
    {
        static constexpr const char *kTechniques[] = {
            "host stub",
            "draw list only",
            "no OpenGL",
        };
        static const app::ConceptInfo kInfo{
            "discord",
            "PSCord",
            "Servers, channels and messages",
            "src/concepts/discord.cpp",
            audio::SoundSet::glass,
            gfx::Color::rgb(0x5865f2),
            kTechniques,
        };
        return kInfo;
    }

    void update(const InputFrame &, float, app::Feedback &) override
    {
    }

    void draw(app::Frame &frame) const override
    {
        frame.scene.rounded_rect({0.0f, 0.0f, 100.0f, 100.0f}, 8.0f, gfx::Color::rgb(0x5865f2));
    }
};

} // namespace

std::unique_ptr<app::Concept> make_discord(app::Context &context)
{
    return std::make_unique<DiscordHostStub>(context);
}

} // namespace hui::concepts
