// PKG Folderizer - Behaviour smoke test for the Sources screen.
// Copyright (C) 2026
// SPDX-License-Identifier: GPL-3.0-or-later

#include "concept_fixture.hpp"
#include "concepts/concepts.hpp"

namespace
{

using hui::Action;

class SourcesScreen : public hui::testing::ConceptFixture
{
  protected:
    std::unique_ptr<hui::app::Concept> design_ = hui::concepts::make_sources(context_);
};

TEST_F(SourcesScreen, EnterAndDraw)
{
    design_->enter();
    send(*design_, idle(), 0.5f);
    expect_drawable(*design_);
}

TEST_F(SourcesScreen, FilterButtonDoesNotCrash)
{
    design_->enter();
    send(*design_, press(Action::west), 0.2f);
    expect_drawable(*design_);
}

} // namespace
