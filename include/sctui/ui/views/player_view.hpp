#pragma once

#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>
#include "sctui/models/state.hpp"

namespace sctui::ui::views {

class PlayerView {
public:
    explicit PlayerView(models::AppState& state);

    ftxui::Component get_component();
    ftxui::Element render(bool is_focused);

private:
    models::AppState& state_;
    ftxui::Component component_;

    ftxui::Element render_progress_bar();
    ftxui::Element render_waveform_or_meter();
    ftxui::Element render_controls();
    ftxui::Element render_volume();
};

} // namespace sctui::ui::views
