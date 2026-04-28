#include <M5Cardputer.h>

void setup() {
    auto cfg = M5.config();
    M5Cardputer.begin(cfg);

    M5Cardputer.Display.setRotation(1);
    M5Cardputer.Display.fillScreen(BLACK);
    M5Cardputer.Display.setTextColor(WHITE, BLACK);
    M5Cardputer.Display.setTextDatum(middle_center);
    M5Cardputer.Display.setTextSize(2);
    M5Cardputer.Display.drawString(
        "Hello, Waxwing",
        M5Cardputer.Display.width() / 2,
        M5Cardputer.Display.height() / 2);
}

void loop() {
    M5Cardputer.update();
    delay(50);
}
