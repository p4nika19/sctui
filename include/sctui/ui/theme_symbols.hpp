#pragma once

#include <string>
#include "sctui/core/config.hpp"

namespace sctui::ui {

struct ThemeSymbols {
    // Branding & Navigation
    std::string logo;          // ☁ / ☁ / [SC]
    std::string search;        // 🔍 / ⌕ / [/]
    std::string stream;        // 📡 / ◎ / [*]
    std::string likes;         // 💖 / ♥ / <3>
    std::string playlists;     // 📁 / ≡ / [=]
    std::string queue;         // 📋 / ≣ / [#]
    std::string account;       // 👤 / ☺ / [@]
    std::string settings;      // ⚙ / ⛭ / [*]

    // Playback state & badges
    std::string play;          // ▶ / ▶ / >
    std::string pause;         // ⏸ / ⏸ / ||
    std::string buffering;     // ⟳ / ⟳ / ~
    std::string loading;       // ◌ / ◌ / ...
    std::string finished;      // ✔ / ✓ / [V]
    std::string error;         // ✖ / ✗ / [!]
    std::string stopped;       // ■ / ■ / [#]
    std::string shuffle;       // 🔀 / ⇄ / [~]
    std::string repeat_off;    // 🔁 OFF / → OFF / [-] OFF
    std::string repeat_one;    // 🔂 ONE / ↺ ONE / [1] ONE
    std::string repeat_all;    // 🔁 ALL / ↻ ALL / [R] ALL
    std::string volume;        // 🔊 / ☊ / (Vol)
    std::string muted;         // 🔇 / ☊x / [MUTED]

    // Action buttons & sections
    std::string key;           // 🔑 / ⚿ / [Key]
    std::string clean;         // 🧹 / ⎚ / [Clr]
    std::string save;          // 💾 / ⛁ / [Save]
    std::string network;       // 🌐 / ⊕ / [Net]
    std::string graphics;      // 🖼 / ▧ / [Gfx]
    std::string audio;         // 🔊 / ☊ / [Aud]
    std::string group;         // 👥 / ☺☺ / [++]
    std::string note;          // ♫ / ♪ / >
    std::string keyboard;      // ⌨ / ⌨ / [?]

    static const ThemeSymbols& get(core::IconStyle style) noexcept {
        static const ThemeSymbols kEmoji{
            .logo = "☁",
            .search = "🔍",
            .stream = "📡",
            .likes = "💖",
            .playlists = "📁",
            .queue = "📋",
            .account = "👤",
            .settings = "🔧",
            .play = "▶",
            .pause = "⏸",
            .buffering = "⟳",
            .loading = "◌",
            .finished = "✔",
            .error = "✖",
            .stopped = "■",
            .shuffle = "🔀",
            .repeat_off = "🔁 OFF",
            .repeat_one = "🔂 ONE",
            .repeat_all = "🔁 ALL",
            .volume = "🔊",
            .muted = "🔇 MUTED",
            .key = "🔑",
            .clean = "🧹",
            .save = "💾",
            .network = "🌐",
            .graphics = "🖼",
            .audio = "🔊",
            .group = "👥",
            .note = "♫",
            .keyboard = "⌨"
        };

        static const ThemeSymbols kUnicode{
            .logo = "☁",
            .search = "⌕",
            .stream = "◎",
            .likes = "♥",
            .playlists = "≡",
            .queue = "≣",
            .account = "☺",
            .settings = "⛭",
            .play = "▶",
            .pause = "⏸",
            .buffering = "⟳",
            .loading = "◌",
            .finished = "✓",
            .error = "✗",
            .stopped = "■",
            .shuffle = "⇄",
            .repeat_off = "→ OFF",
            .repeat_one = "↺ ONE",
            .repeat_all = "↻ ALL",
            .volume = "☊",
            .muted = "☊x MUTED",
            .key = "⚿",
            .clean = "⎚",
            .save = "⛁",
            .network = "⊕",
            .graphics = "▧",
            .audio = "☊",
            .group = "☺☺",
            .note = "♪",
            .keyboard = "⌨"
        };

        static const ThemeSymbols kDefaultAscii{
            .logo = "[SC]",
            .search = "[/]",
            .stream = "[*]",
            .likes = "<3>",
            .playlists = "[=]",
            .queue = "[#]",
            .account = "[@]",
            .settings = "[*]",
            .play = ">",
            .pause = "||",
            .buffering = "~",
            .loading = "...",
            .finished = "[V]",
            .error = "[!]",
            .stopped = "[#]",
            .shuffle = "[~]",
            .repeat_off = "[-] OFF",
            .repeat_one = "[1] ONE",
            .repeat_all = "[R] ALL",
            .volume = "(Vol)",
            .muted = "[MUTED]",
            .key = "[Key]",
            .clean = "[Clr]",
            .save = "[Save]",
            .network = "[Net]",
            .graphics = "[Gfx]",
            .audio = "[Aud]",
            .group = "[++]",
            .note = ">",
            .keyboard = "[?]"
        };

        switch (style) {
            case core::IconStyle::Unicode: return kUnicode;
            case core::IconStyle::DefaultAscii: return kDefaultAscii;
            case core::IconStyle::Emoji: default: return kEmoji;
        }
    }
};

} // namespace sctui::ui
