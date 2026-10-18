// Copyright 2026 Zeste contributors (fork of the Azahar Emulator Project)
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

#include <deque>
#include <iostream>
#include <mutex>
#include <fmt/format.h>
#include "common/logging/log.h"
#include "core/core.h"
#include "core/frontend/applets/swkbd.h"
#include "core/zeste_keyboard.h"

namespace Zeste::Keyboard {

namespace {

struct Answer {
    bool cancel = false;
    std::string text;
};

void Print(const std::string& line) {
    LOG_INFO(Zeste, "{}", line);
    std::cout << line << std::endl;
}

class HeadlessKeyboard final : public Frontend::SoftwareKeyboard {
public:
    void Execute(const Frontend::KeyboardConfig& config_) override {
        SoftwareKeyboard::Execute(config_);
        std::scoped_lock lock{mutex};
        waiting = true;
        Print(fmt::format(
            "Zeste keyboard: the game asks for text{}{}; answer with \"keyboard <text>\"{}",
            config.hint_text.empty() ? "" : fmt::format(" (hint \"{}\")", config.hint_text),
            config.max_text_length ? fmt::format(", up to {} characters", config.max_text_length)
                                   : "",
            HasCancel() ? " or \"keyboard-cancel\"" : ""));
        TryAnswer();
    }

    void ShowError(const std::string& error) override {
        Print(fmt::format("Zeste keyboard: the game rejected the text: {}", error));
    }

    void Push(Answer answer) {
        std::scoped_lock lock{mutex};
        pending.push_back(std::move(answer));
        if (waiting) {
            TryAnswer();
        } else {
            LOG_INFO(Zeste, "keyboard answer queued for the next request");
        }
    }

private:
    bool HasCancel() const {
        return config.button_config == Frontend::ButtonConfig::Dual ||
               config.button_config == Frontend::ButtonConfig::Triple;
    }

    /// The OK button's index for the current configuration.
    u8 OkButton() const {
        return config.button_config == Frontend::ButtonConfig::None
                   ? 0
                   : static_cast<u8>(config.button_config);
    }

    // Called with the mutex held.
    void TryAnswer() {
        while (waiting && !pending.empty()) {
            const Answer answer = pending.front();
            pending.pop_front();
            if (answer.cancel && !HasCancel()) {
                Print("Zeste keyboard: this keyboard has no Cancel button; still waiting");
                continue;
            }
            const u8 button = answer.cancel ? 0 : OkButton();
            const auto error = Finalize(answer.cancel ? std::string{} : answer.text, button);
            if (error != Frontend::ValidationError::None) {
                Print(fmt::format("Zeste keyboard: \"{}\" is not accepted ({}); still waiting",
                                  answer.text, Describe(error)));
                continue;
            }
            waiting = false;
            Print(answer.cancel ? std::string{"Zeste keyboard: Cancel"}
                                : fmt::format("Zeste keyboard: typed \"{}\"", answer.text));
        }
    }

    static const char* Describe(Frontend::ValidationError error) {
        using Frontend::ValidationError;
        switch (error) {
        case ValidationError::FixedLengthRequired:
            return "wrong length";
        case ValidationError::MaxLengthExceeded:
            return "too long";
        case ValidationError::BlankInputNotAllowed:
            return "blank text not allowed";
        case ValidationError::EmptyInputNotAllowed:
            return "empty text not allowed";
        default:
            return "rejected by the keyboard's filters";
        }
    }

    std::mutex mutex;
    bool waiting = false;
    std::deque<Answer> pending;
};

std::shared_ptr<HeadlessKeyboard> g_keyboard;

} // Anonymous namespace

void RegisterHeadless(Core::System& system) {
    g_keyboard = std::make_shared<HeadlessKeyboard>();
    system.RegisterSoftwareKeyboard(g_keyboard);
}

void Type(const std::string& text) {
    if (g_keyboard) {
        g_keyboard->Push({false, text});
    } else {
        LOG_WARNING(Zeste, "keyboard: only available in headless mode");
    }
}

void Cancel() {
    if (g_keyboard) {
        g_keyboard->Push({true, {}});
    } else {
        LOG_WARNING(Zeste, "keyboard-cancel: only available in headless mode");
    }
}

} // namespace Zeste::Keyboard
