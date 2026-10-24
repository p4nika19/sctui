#pragma once

#include <ftxui/dom/elements.hpp>
#include "sctui/models/state.hpp"

namespace sctui::ui::views {

class HelpPopup {
public:
    static ftxui::Element render(const models::AppState& state);
};

} // namespace sctui::ui::views
