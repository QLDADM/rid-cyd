// Panel dotykowy XPT2046 na CYD - obsługa programowa (bit-bang) na własnych pinach płytki,
// żeby nie zajmować magistrali SPI karty SD. Kalibracja 2-punktowa zapisywana w NVS.
#pragma once

#define T_CLK 25
#define T_DIN 32
#define T_DOUT 39
#define T_CS 33
#define T_IRQ 36  // stan niski = dotyk
#define T_Z_MIN 350  // nacisk uznawany za dotyk

struct TouchCal {
  int16_t x0, x1, y0, y1;  // surowe odczyty odpowiadające krawędziom ekranu (0 i 319 / 0 i 239)
  uint8_t swap;            // 1 = osie panelu zamienione względem ekranu
};
static TouchCal touchCal = {200, 3700, 240, 3800, 0};  // typowe wartości dla CYD

static void touchBegin() {
  pinMode(T_CLK, OUTPUT);
  pinMode(T_DIN, OUTPUT);
  pinMode(T_CS, OUTPUT);
  pinMode(T_DOUT, INPUT);
  pinMode(T_IRQ, INPUT);
  digitalWrite(T_CS, HIGH);
  digitalWrite(T_CLK, LOW);
}

// jak w sprawdzonych bibliotekach dla CYD (XPT2046 bit-bang): 8 bitów polecenia, potem 12 bitów wyniku
static uint16_t touchXfer(uint8_t cmd) {
  for (int i = 7; i >= 0; i--) {
    digitalWrite(T_DIN, (cmd >> i) & 1);
    digitalWrite(T_CLK, HIGH);
    delayMicroseconds(5);
    digitalWrite(T_CLK, LOW);
    delayMicroseconds(5);
  }
  digitalWrite(T_DIN, LOW);  // inaczej kontroler bierze "1" za bit startu nowego polecenia
  uint16_t v = 0;
  for (int i = 11; i >= 0; i--) {
    digitalWrite(T_CLK, HIGH);
    delayMicroseconds(5);
    digitalWrite(T_CLK, LOW);
    delayMicroseconds(5);
    v |= digitalRead(T_DOUT) << i;
  }
  return v;
}

// surowy odczyt; false, gdy nikt nie dotyka
static bool touchRaw(int &rx, int &ry) {
  digitalWrite(T_CS, LOW);
  int z1 = touchXfer(0xB0), z2 = touchXfer(0xC0);
  int z = z1 + 4095 - z2;
  if (z < T_Z_MIN) {
    digitalWrite(T_CS, HIGH);
    return false;
  }
  long sx = 0, sy = 0;
  touchXfer(0xD0);  // pierwszy odczyt bywa zaszumiony
  for (int i = 0; i < 4; i++) {
    sx += touchXfer(0xD0);
    sy += touchXfer(0x90);
  }
  digitalWrite(T_CS, HIGH);
  rx = sx / 4;
  ry = sy / 4;
  return true;
}

// współrzędne ekranu (poziomo 320x240)
static bool touchRead(int &x, int &y) {
  int rx, ry;
  if (!touchRaw(rx, ry)) return false;
  int a = touchCal.swap ? ry : rx, b = touchCal.swap ? rx : ry;
  x = constrain(map(a, touchCal.x0, touchCal.x1, 0, 319), 0, 319);
  y = constrain(map(b, touchCal.y0, touchCal.y1, 0, 239), 0, 239);
  return true;
}
