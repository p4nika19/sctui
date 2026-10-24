#pragma once

#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>
#include <functional>
#include "sctui/models/state.hpp"
#include "sctui/core/config.hpp"

namespace sctui::ui::views {

class SidebarView {
public:
    SidebarView(
        models::AppState& state,
        const core::Config& config,
        std::function<void(models::ViewMode)> on_view_selected
    );

    ftxui::Component get_component();
    ftxui::Element render(bool is_focused);
    void set_selected_row(int row);

private:
    models::AppState& state_;
    const core::Config& config_;
    std::function<void(models::ViewMode)> on_view_selected_;
    ftxui::Component component_;
    int selected_row_{0};
};

} // namespace sctui::ui::views
