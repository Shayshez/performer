#include "ConfirmationPage.h"

#include "ui/painters/WindowPainter.h"

ConfirmationPage::ConfirmationPage(PageManager &manager, PageContext &context) :
    BasePage(manager, context)
{}

void ConfirmationPage::show(const char *text, ResultCallback callback) {
    _text = text;
    _callback = callback;
    Page::show();
}

void ConfirmationPage::enter() {
}

void ConfirmationPage::exit() {
}

void ConfirmationPage::draw(Canvas &canvas) {

    WindowPainter::clear(canvas);

    canvas.setFont(Font::Tiny);
    canvas.setBlendMode(BlendMode::Set);
    canvas.setColor(0xf);

    canvas.drawTextCentered(0, 32 - 4, Width, 8, _text);

    const char *functionNames[] = { nullptr, nullptr, nullptr, "CANCEL", "OK" };
    WindowPainter::drawFunctionKeys(canvas, functionNames, _keyState);
}

void ConfirmationPage::updateLeds(Leds &leds) {
}

void ConfirmationPage::keyDown(KeyEvent &event) {
    const auto &key = event.key();

    if (key.isFunction()) {
        switch (key.function()) {
        case 3: close(false); break;
        case 4: close(true); break;
        }
    }
}

void ConfirmationPage::keyUp(KeyEvent &event) {
}

void ConfirmationPage::encoder(EncoderEvent &event) {
}

void ConfirmationPage::close(bool result) {
    Page::close();
    if (_callback) {
        _callback(result);
    }
}
