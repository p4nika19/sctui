#include "sctui/ui/views/help_popup.hpp"
#include "sctui/ui/theme_symbols.hpp"
#include <ftxui/dom/elements.hpp>

namespace sctui::ui::views {

using namespace ftxui;

Element HelpPopup::render(const models::AppState& state) {
    auto accent = state.get_accent_color();
    const auto& sym = ThemeSymbols::get(state.icon_style);
    auto title = text(" " + sym.keyboard + " SOUNDCLOUD TUI KEYBOARD COMMANDS ") | color(accent) | bold | center;

    auto make_row = [accent](std::string key, std::string desc) {
        return hbox({
            text("  " + key + " ") | color(accent) | bold | size(WIDTH, EQUAL, 20),
            text(desc) | color(Color::White)
        });
    };

    auto section_header = [](std::string sec) {
        return text(" " + sec + " ") | color(Color::GrayLight) | bold;
    };

    Elements content = {
        title,
        separator(),

        section_header("NAVIGATION"),
        make_row("j / k or Up / Down", "Move selection in current panel"),
        make_row("h / l or Left / Right", "Navigate between Sidebar & List"),
        make_row("Enter", "Play selected track / Open view"),
        make_row("Tab / Shift+Tab", "Cycle focus (Search, Nav, List, Player)"),
        make_row("/", "Focus search input"),
        make_row("Esc", "Unfocus search / Return to Track List"),
        make_row("1 .. 7 or S", "Jump view (Search, Stream, Likes, Sets, Queue, Account, Settings)"),
        make_row("g / G", "Jump to top / bottom of list"),

        separatorLight(),
        section_header("PLAYBACK CONTROLS"),
        make_row("f / F", "Toggle Fullscreen Playback view"),
        make_row("Space", "Play / Pause toggle"),
        make_row("n / >", "Next track (in queue or shuffle order)"),
        make_row("p / <", "Previous track (or seek to 00:00 if >3s)"),
        make_row("[ / ] or , / .", "Seek -5s / +5s"),
        make_row("H / L", "Seek -30s / +30s (Large jump)"),
        make_row("+ / -", "Volume up / down"),
        make_row("m", "Mute / Unmute toggle"),
        make_row("s", "Shuffle mode toggle"),
        make_row("r", "Cycle repeat mode (Off -> One -> All)"),

        separatorLight(),
        section_header("QUEUE & LIBRARY"),
        make_row("a", "Add selected track to queue"),
        make_row("A", "Play track next (insert after current)"),
        make_row("l", "Toggle like for selected track"),
        make_row("j / k or J / K", "Reorder track down / up in queue"),
        make_row("Up / Down", "Move selection cursor in queue"),
        make_row("d / x", "Delete selected track from queue"),
        make_row("C", "Clear entire queue"),

        separatorLight(),
        section_header("GENERAL"),
        make_row("F5 / R", "Refresh view / Resume interrupted loading"),
        make_row("Ctrl+R", "Force reload view from scratch"),
        make_row("?", "Toggle this Help modal"),
        make_row("q / Ctrl+C", "Graceful exit and terminal restore"),

        separator(),
        text(" Press [?] or [Esc] to close ") | color(Color::GrayLight) | center
    };

    return vbox(std::move(content))
        | borderRounded
        | color(accent)
        | bgcolor(Color::RGB(10, 10, 12))
        | size(WIDTH, LESS_THAN, 75)
        | clear_under;
}

} // namespace sctui::ui::views
