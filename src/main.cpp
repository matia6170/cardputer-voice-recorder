#include <Arduino.h>

#include "M5Cardputer.h"

void setup() {
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);  // enableKeyboard

    Serial.begin(115200);
    M5Cardputer.Display.setRotation(1);
    M5Cardputer.Display.setTextColor(GREEN);
    M5Cardputer.Display.setTextDatum(middle_center);
    M5Cardputer.Display.setTextFont(&fonts::Orbitron_Light_24);
    M5Cardputer.Display.setTextSize(1);
    M5Cardputer.Display.drawString("Cardputer Template", M5Cardputer.Display.width() / 2,
                                   M5Cardputer.Display.height() / 2 - 40);
}

void loop() {
    M5Cardputer.update();
    // Refresh image if Button G0 (Enter key on Cardputer) is pressed
    if (M5Cardputer.Keyboard.isPressed()) {
        if (M5Cardputer.Keyboard.isKeyPressed(KEY_ENTER)) {
            M5Cardputer.Display.fillScreen(GREEN);
            M5Cardputer.Display.setTextColor(WHITE);
            M5Cardputer.Display.setTextDatum(middle_center);
            M5Cardputer.Display.setTextFont(&fonts::Orbitron_Light_24);
            M5Cardputer.Display.setTextSize(1);
            M5Cardputer.Display.drawString("Button Pressed!", M5Cardputer.Display.width() / 2,
                                           M5Cardputer.Display.height() / 2 - 40);
        }
    }
}