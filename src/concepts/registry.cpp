// PS5 Discord - Screen list.
// Copyright (C) 2026 Desstann
// SPDX-License-Identifier: GPL-3.0-or-later

#include "concepts/concepts.hpp"

namespace hui::app
{

std::span<const ConceptFactory> concept_registry()
{
    static constexpr ConceptFactory kFactories[] = {
        concepts::make_discord,
    };
    return kFactories;
}

} // namespace hui::app
