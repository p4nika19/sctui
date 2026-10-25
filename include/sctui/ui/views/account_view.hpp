#pragma once

#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>
#include <functional>
#include <vector>
#include "sctui/models/track.hpp"
#include "sctui/models/state.hpp"
#include "sctui/core/config.hpp"

namespace sctui::ui::views {

class AccountView {
public:
    using PlayCallback = std::function<void(const models::Track& track, size_t index)>;
    using QueueCallback = std::function<void(const models::Track& track, bool play_next)>;
    using LikeCallback = std::function<void(const models::Track& track)>;
    using ExtractCookiesCallback = std::function<void()>;

    AccountView(
        models::AppState& state,
        const core::Config& config,
        PlayCallback on_play,
        QueueCallback on_queue,
        LikeCallback on_like,
        ExtractCookiesCallback on_extract_cookies
    );

    ftxui::Component get_component();
    ftxui::Element render(bool is_focused);

    void scroll_up();
    void scroll_down();
    void select_row(int visible_row);
    void queue_selected(bool play_next);

private:
    models::AppState& state_;
    const core::Config& config_;
    PlayCallback on_play_;
    QueueCallback on_queue_;
    LikeCallback on_like_;
    ExtractCookiesCallback on_extract_cookies_;

    ftxui::Component component_;
    int scroll_offset_{0};
};

} // namespace sctui::ui::views
