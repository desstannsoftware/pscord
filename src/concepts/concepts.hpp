// PS5 Discord - Screen factories.
// Copyright (C) 2026 Desstann
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "app/concept.hpp"

#include <memory>

namespace hui::concepts
{

std::unique_ptr<app::Concept> make_discord(app::Context &context);

} // namespace hui::concepts
