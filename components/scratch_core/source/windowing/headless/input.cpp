#include <input.hpp>
#include <iostream>
#include <os.hpp>
#include <runtime.hpp>

// External input callback (set by main to feed gamepad input)
static void (*s_external_input_cb)() = nullptr;

extern "C" void render_set_input_callback(void (*cb)()) {
    s_external_input_cb = cb;
}

std::vector<int> Input::getTouchPosition() {
    return {0, 0};
}

void Input::getInput() {
    inputButtons.clear();
    if (s_external_input_cb) {
        s_external_input_cb();
    }
    if (!inputButtons.empty()) {
        inputButtons.push_back("any");
    }
    BlockExecutor::executeKeyHats();
    BlockExecutor::doSpriteClicking();
}

std::string Input::openSoftwareKeyboard(const char *hintText) {
    Log::log(std::string(hintText));
    std::string input;
    std::getline(std::cin, input);
    return input;
}