#pragma once

#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>
#include "sctui/models/state.hpp"

namespace sctui::ui::views {

class HeaderView {
public:
    explicit HeaderView(models::AppState& state, std::function<void(const std::string&)> on_search_submit);

    ftxui::Component get_component();
    ftxui::Element render();

    void focus_input();
    void clear();

private:
    models::AppState& state_;
    std::function<void(const std::string&)> on_search_submit_;
    ftxui::Component input_box_;
};

} // namespace sctui::ui::views
