#pragma once

#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>
#include <functional>
#include "sctui/models/state.hpp"

namespace sctui::ui::views {

class QueueView {
public:
    using PlayFromQueueCallback = std::function<void(size_t index)>;

    explicit QueueView(models::AppState& state, PlayFromQueueCallback on_play_from_queue);

    ftxui::Component get_component();
    ftxui::Element render(bool is_focused);

    void move_selected_up();
    void move_selected_down();
    void remove_selected();
    void clear_queue();
    void scroll_up();
    void scroll_down();
    void select_row(int visible_row);

    [[nodiscard]] int get_selected_index() const;

private:
    models::AppState& state_;
    PlayFromQueueCallback on_play_from_queue_;
    ftxui::Component component_;
    int selected_index_{0};
    int scroll_offset_{0};
    uint64_t last_active_track_id_{0};
};

} // namespace sctui::ui::views
