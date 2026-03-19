#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <Preferences.h>
#include <SPI.h>
#include <XPT2046_Touchscreen.h>

#include <math.h>

#ifndef BATTERY_ADC_PIN
#define BATTERY_ADC_PIN -1
#endif

#ifndef BATTERY_DIVIDER_RATIO
#define BATTERY_DIVIDER_RATIO 2.0f
#endif

#ifndef BATTERY_VOLT_MIN
#define BATTERY_VOLT_MIN 3.30f
#endif

#ifndef BATTERY_VOLT_MAX
#define BATTERY_VOLT_MAX 4.20f
#endif

// Pin map and timings from CrowPanel 4.3" wiki page.
Arduino_ESP32RGBPanel *bus = new Arduino_ESP32RGBPanel(
  40, 41, 39, 42,
  45, 48, 47, 21, 14,
  5, 6, 7, 15, 16, 4,
  8, 3, 46, 9, 1,
  0, 8, 4, 43,
  0, 8, 4, 12,
  1, 7000000, false,
  0, 0);

Arduino_RGB_Display *gfx = new Arduino_RGB_Display(480, 272, bus, 0, true);
SPIClass touchSpi(FSPI);
XPT2046_Touchscreen touch(0, 36);
Preferences prefs;

constexpr uint16_t COLOR_BLACK = 0x0000;
constexpr uint16_t COLOR_WHITE = 0xFFFF;
constexpr uint16_t COLOR_RED = 0xF800;
constexpr uint16_t COLOR_GREEN = 0x07E0;
constexpr uint16_t COLOR_BLUE = 0x001F;
constexpr uint16_t COLOR_YELLOW = 0xFFE0;
constexpr uint16_t COLOR_CYAN = 0x07FF;
constexpr uint16_t COLOR_MAGENTA = 0xF81F;
constexpr uint16_t COLOR_ORANGE = 0xFD20;
constexpr uint16_t COLOR_GRAY = 0x8410;

constexpr int kScreenWidth = 480;
constexpr int kScreenHeight = 272;
constexpr int kHeaderH = 34;
constexpr int kButtonBarH = 46;
constexpr int kSceneTop = kHeaderH;
constexpr int kSceneBottom = kScreenHeight - kButtonBarH;
constexpr int kSceneH = kSceneBottom - kSceneTop;

constexpr int kButtonY = kScreenHeight - kButtonBarH + 6;
constexpr int kButtonW = 90;
constexpr int kButtonH = 34;
constexpr int kButtonGap = 6;
constexpr int kButtonStartX = 6;

constexpr uint8_t kFpsOptions[] = {15, 30, 45, 60};
constexpr size_t kFpsOptionsCount = sizeof(kFpsOptions) / sizeof(kFpsOptions[0]);

constexpr uint8_t kDefaultBrightnessPct = 80;
constexpr uint8_t kDefaultFpsIndex = 1;

constexpr int GPIO_BOOT_CONTROL = 37;

enum TouchAppMode
{
  TOUCH_MODE_CALIBRATION,
  TOUCH_MODE_UI,
};

enum DemoScene
{
  SCENE_PALETTE,
  SCENE_SHAPES,
  SCENE_BARS,
  SCENE_GRADIENT,
  SCENE_SETTINGS,
};

struct CalibrationTarget
{
  int16_t x;
  int16_t y;
  const char *label;
};

struct UiButton
{
  int16_t x;
  int16_t y;
  int16_t w;
  int16_t h;
  const char *label;
  DemoScene scene;
};

struct UiSettings
{
  uint8_t brightnessPct;
  uint8_t fpsIndex;
  bool powerSaveEnabled;
};

CalibrationTarget g_targets[] = {
  {40, 74, "alto sx"},
  {440, 74, "alto dx"},
  {440, 198, "basso dx"},
  {40, 198, "basso sx"},
};

UiButton g_buttons[] = {
  {kButtonStartX + ((kButtonW + kButtonGap) * 0), kButtonY, kButtonW, kButtonH, "Palette", SCENE_PALETTE},
  {kButtonStartX + ((kButtonW + kButtonGap) * 1), kButtonY, kButtonW, kButtonH, "Shapes", SCENE_SHAPES},
  {kButtonStartX + ((kButtonW + kButtonGap) * 2), kButtonY, kButtonW, kButtonH, "Bars", SCENE_BARS},
  {kButtonStartX + ((kButtonW + kButtonGap) * 3), kButtonY, kButtonW, kButtonH, "Gradient", SCENE_GRADIENT},
  {kButtonStartX + ((kButtonW + kButtonGap) * 4), kButtonY, kButtonW, kButtonH, "Settings", SCENE_SETTINGS},
};

TouchAppMode g_mode = TOUCH_MODE_CALIBRATION;
DemoScene g_scene = SCENE_PALETTE;
UiSettings g_settings = {kDefaultBrightnessPct, kDefaultFpsIndex, false};

unsigned long g_lastFrameMs = 0;
unsigned long g_lastTouchLogMs = 0;
unsigned long g_uiFrame = 0;
unsigned long g_lastBatteryReadMs = 0;
bool g_uiInit = false;
bool g_lastPressed = false;
int16_t g_lastX = kScreenWidth / 2;
int16_t g_lastY = kScreenHeight / 2;
int16_t g_filteredX = kScreenWidth / 2;
int16_t g_filteredY = kScreenHeight / 2;
bool g_hasFilteredTouch = false;

uint8_t g_calStep = 0;
bool g_calPressActive = false;
bool g_waitCalibrationRelease = false;
uint32_t g_calXSum = 0;
uint32_t g_calYSum = 0;
uint16_t g_calSamples = 0;
uint16_t g_calRawX[4] = {0, 0, 0, 0};
uint16_t g_calRawY[4] = {0, 0, 0, 0};

int32_t g_rawEdgeLeft = 3900;
int32_t g_rawEdgeRight = 100;
int32_t g_rawEdgeTop = 120;
int32_t g_rawEdgeBottom = 3900;

bool g_sceneNeedsRedraw = true;
constexpr int kBarsCount = 4;
int g_prevBarTop[kBarsCount] = {kSceneBottom, kSceneBottom, kSceneBottom, kSceneBottom};
int g_prevBarHeight[kBarsCount] = {0, 0, 0, 0};

bool g_batteryMonitorAvailable = (BATTERY_ADC_PIN >= 0);
bool g_batteryPresent = false;
uint8_t g_batteryPercent = 0;
float g_batteryVoltage = 0.0f;

bool isValidCalibration(int32_t left, int32_t right, int32_t top, int32_t bottom)
{
  bool xOk = abs(left - right) > 500;
  bool yOk = abs(top - bottom) > 500;
  return xOk && yOk;
}

void saveCalibration()
{
  prefs.begin("crowpanel", false);
  prefs.putBool("cal_ok", true);
  prefs.putInt("cal_l", g_rawEdgeLeft);
  prefs.putInt("cal_r", g_rawEdgeRight);
  prefs.putInt("cal_t", g_rawEdgeTop);
  prefs.putInt("cal_b", g_rawEdgeBottom);
  prefs.end();
}

bool loadCalibration()
{
  prefs.begin("crowpanel", true);
  bool ok = prefs.getBool("cal_ok", false);
  int32_t left = prefs.getInt("cal_l", g_rawEdgeLeft);
  int32_t right = prefs.getInt("cal_r", g_rawEdgeRight);
  int32_t top = prefs.getInt("cal_t", g_rawEdgeTop);
  int32_t bottom = prefs.getInt("cal_b", g_rawEdgeBottom);
  prefs.end();

  if (!ok || !isValidCalibration(left, right, top, bottom))
  {
    return false;
  }

  g_rawEdgeLeft = left;
  g_rawEdgeRight = right;
  g_rawEdgeTop = top;
  g_rawEdgeBottom = bottom;
  return true;
}

void resetToFactoryDefaults()
{
  Serial.println(">>> RESET TO FACTORY DEFAULTS STARTING <<<");
  
  g_settings.brightnessPct = kDefaultBrightnessPct;
  g_settings.fpsIndex = kDefaultFpsIndex;
  g_settings.powerSaveEnabled = false;
  g_rawEdgeLeft = 3900;
  g_rawEdgeRight = 100;
  g_rawEdgeTop = 120;
  g_rawEdgeBottom = 3900;
  
  prefs.begin("crowpanel", false);
  prefs.clear();
  prefs.putBool("cal_ok", false);
  prefs.putUChar("set_brt", g_settings.brightnessPct);
  prefs.putUChar("set_fps", g_settings.fpsIndex);
  prefs.putBool("set_pwr", g_settings.powerSaveEnabled);
  prefs.end();
  
  Serial.printf("Factory defaults restored: brt=%d%% fps=%u pwr_save=%d; calibration cleared\n",
    g_settings.brightnessPct, kFpsOptions[g_settings.fpsIndex], g_settings.powerSaveEnabled);
  Serial.println(">>> RESET COMPLETE <<<");
}

void saveUiSettings()
{
  prefs.begin("crowpanel", false);
  prefs.putUChar("set_brt", g_settings.brightnessPct);
  prefs.putUChar("set_fps", g_settings.fpsIndex);
  prefs.putBool("set_pwr", g_settings.powerSaveEnabled);
  prefs.end();
}

void loadUiSettings()
{
  prefs.begin("crowpanel", true);
  g_settings.brightnessPct = prefs.getUChar("set_brt", kDefaultBrightnessPct);
  g_settings.fpsIndex = prefs.getUChar("set_fps", kDefaultFpsIndex);
  g_settings.powerSaveEnabled = prefs.getBool("set_pwr", false);
  prefs.end();

  if (g_settings.brightnessPct > 100)
  {
    g_settings.brightnessPct = kDefaultBrightnessPct;
  }

  if (g_settings.fpsIndex >= kFpsOptionsCount)
  {
    g_settings.fpsIndex = kDefaultFpsIndex;
  }
}

unsigned long getFrameIntervalMs()
{
  uint8_t fps = kFpsOptions[g_settings.fpsIndex];
  if (g_settings.powerSaveEnabled && g_batteryPresent)
  {
    fps = 15;
  }
  if (fps == 0)
  {
    fps = 1;
  }
  return 1000UL / fps;
}

void applyBacklightBrightness()
{
  uint8_t pct = g_settings.brightnessPct;
  if (g_settings.powerSaveEnabled && g_batteryPresent && pct > 35)
  {
    pct = 35;
  }

  uint8_t pwm = map(pct, 0, 100, 0, 255);
  analogWrite(2, pwm);
  digitalWrite(38, HIGH);
}

uint16_t clampMapBidirectional(int32_t value, int32_t inStart, int32_t inEnd, int32_t outStart, int32_t outEnd)
{
  if (inStart == inEnd)
  {
    return static_cast<uint16_t>(outStart);
  }

  bool asc = inStart < inEnd;
  if (asc)
  {
    if (value <= inStart) return static_cast<uint16_t>(outStart);
    if (value >= inEnd) return static_cast<uint16_t>(outEnd);
  }
  else
  {
    if (value >= inStart) return static_cast<uint16_t>(outStart);
    if (value <= inEnd) return static_cast<uint16_t>(outEnd);
  }

  return static_cast<uint16_t>(map(value, inStart, inEnd, outStart, outEnd));
}

int32_t extrapolateRawEdge(int32_t rawStart, int32_t rawEnd, int32_t screenStart, int32_t screenEnd, int32_t desiredScreen)
{
  int32_t span = screenEnd - screenStart;
  if (span == 0)
  {
    return rawStart;
  }
  return rawStart + ((desiredScreen - screenStart) * (rawEnd - rawStart)) / span;
}

uint16_t mapTouchX(uint16_t rawX)
{
  return clampMapBidirectional(rawX, g_rawEdgeLeft, g_rawEdgeRight, 0, kScreenWidth - 1);
}

uint16_t mapTouchY(uint16_t rawY)
{
  return clampMapBidirectional(rawY, g_rawEdgeTop, g_rawEdgeBottom, 0, kScreenHeight - 1);
}

bool sampleTouch(uint16_t &rawX, uint16_t &rawY, uint16_t &pressure)
{
  if (!touch.touched())
  {
    return false;
  }

  // Average multiple readings to reduce jitter when the finger is still.
  uint32_t sx = 0;
  uint32_t sy = 0;
  uint32_t sz = 0;
  uint8_t samples = 0;
  for (uint8_t i = 0; i < 5; i++)
  {
    TS_Point p = touch.getPoint();
    sx += static_cast<uint16_t>(p.x);
    sy += static_cast<uint16_t>(p.y);
    sz += static_cast<uint16_t>(p.z);
    samples++;
    delayMicroseconds(250);
  }

  rawX = static_cast<uint16_t>(sx / samples);
  rawY = static_cast<uint16_t>(sy / samples);
  pressure = static_cast<uint16_t>(sz / samples);
  return true;
}

void drawWelcomeScreen()
{
  gfx->fillScreen(COLOR_BLACK);

  for (int y = 0; y < kScreenHeight; y += 2)
  {
    uint16_t b = (y * 31) / kScreenHeight;
    uint16_t g = 10 + ((kScreenHeight - y) * 20) / kScreenHeight;
    uint16_t color = (g << 5) | b;
    gfx->drawLine(0, y, kScreenWidth - 1, y, color);
  }

  gfx->drawRoundRect(28, 66, 424, 140, 14, COLOR_WHITE);
  gfx->setTextColor(COLOR_WHITE);
  gfx->setTextSize(2);
  gfx->setCursor(118, 108);
  gfx->println("TWISTER");

  gfx->setTextSize(2);
  gfx->setCursor(102, 138);
  gfx->println("sailingcomputer");

  gfx->setTextSize(1);
  gfx->setCursor(160, 178);
  gfx->println("Loading interface...");
}

void updateBatteryStatus()
{
  if (!g_batteryMonitorAvailable)
  {
    g_batteryPresent = false;
    return;
  }

  unsigned long now = millis();
  if (now - g_lastBatteryReadMs < 1500)
  {
    return;
  }
  g_lastBatteryReadMs = now;

  uint32_t sum = 0;
  for (int i = 0; i < 8; i++)
  {
    sum += analogRead(BATTERY_ADC_PIN);
    delay(1);
  }
  float raw = static_cast<float>(sum) / 8.0f;
  float adcVolt = (raw / 4095.0f) * 3.3f;
  float batteryVolt = adcVolt * BATTERY_DIVIDER_RATIO;

  g_batteryVoltage = batteryVolt;
  g_batteryPresent = (batteryVolt > 2.8f && batteryVolt < 4.5f);

  if (!g_batteryPresent)
  {
    g_batteryPercent = 0;
    return;
  }

  float pct = (batteryVolt - BATTERY_VOLT_MIN) * 100.0f / (BATTERY_VOLT_MAX - BATTERY_VOLT_MIN);
  if (pct < 0.0f) pct = 0.0f;
  if (pct > 100.0f) pct = 100.0f;
  g_batteryPercent = static_cast<uint8_t>(pct);
}

void drawCalibrationTarget(int16_t x, int16_t y, uint16_t color)
{
  gfx->fillCircle(x, y, 4, color);
  gfx->drawCircle(x, y, 8, color);
  gfx->drawLine(x - 14, y, x + 14, y, color);
  gfx->drawLine(x, y - 14, x, y + 14, color);
}

void drawCalibrationScreen()
{
  gfx->fillScreen(COLOR_BLACK);
  gfx->drawRect(0, 0, kScreenWidth, kScreenHeight, COLOR_WHITE);
  gfx->fillRect(0, 0, kScreenWidth, 60, COLOR_BLACK);
  gfx->drawLine(0, 60, kScreenWidth - 1, 60, COLOR_WHITE);

  gfx->setTextColor(COLOR_WHITE);
  gfx->setTextSize(2);
  gfx->setCursor(12, 10);
  gfx->println("Touch Calibration");
  gfx->setTextSize(1);
  gfx->setCursor(12, 36);
  gfx->println("Tocca il target evidenziato e rilascia.");

  for (uint8_t i = 0; i < 4; i++)
  {
    uint16_t c = (i < g_calStep) ? COLOR_GREEN : COLOR_GRAY;
    if (i == g_calStep)
    {
      c = COLOR_YELLOW;
    }
    drawCalibrationTarget(g_targets[i].x, g_targets[i].y, c);
  }

  gfx->fillRect(12, 218, 456, 40, COLOR_BLACK);
  gfx->drawRect(12, 218, 456, 40, COLOR_WHITE);
  gfx->setTextColor(COLOR_WHITE);
  gfx->setCursor(20, 230);
  gfx->printf("Step %u/4: tocca %s", g_calStep + 1, g_targets[g_calStep].label);
  gfx->setCursor(20, 244);
  gfx->println("Mantieni ~0.5s per miglior precisione");
}

void resetBarsState()
{
  for (int i = 0; i < kBarsCount; i++)
  {
    g_prevBarTop[i] = kSceneBottom;
    g_prevBarHeight[i] = 0;
  }
}

void drawUiChrome()
{
  gfx->fillScreen(COLOR_BLACK);
  gfx->fillRect(0, 0, kScreenWidth, kHeaderH, COLOR_BLACK);
  gfx->drawLine(0, kHeaderH - 1, kScreenWidth - 1, kHeaderH - 1, COLOR_WHITE);
  gfx->fillRect(0, kSceneBottom, kScreenWidth, kButtonBarH, COLOR_BLACK);
  gfx->drawLine(0, kSceneBottom, kScreenWidth - 1, kSceneBottom, COLOR_WHITE);

  gfx->setTextColor(COLOR_WHITE);
  gfx->setTextSize(2);
  gfx->setCursor(10, 8);
  gfx->println("Step 4 - Touch UI");

  gfx->setTextSize(1);
  gfx->setTextColor(COLOR_GREEN);
  gfx->setCursor(236, 12);
  gfx->println("Calibrazione salvata");
}

void drawButtons()
{
  for (uint8_t i = 0; i < 5; i++)
  {
    UiButton &b = g_buttons[i];
    bool selected = (b.scene == g_scene);
    uint16_t bg = selected ? COLOR_CYAN : COLOR_GRAY;
    uint16_t fg = selected ? COLOR_BLACK : COLOR_WHITE;
    gfx->fillRoundRect(b.x, b.y, b.w, b.h, 6, bg);
    gfx->drawRoundRect(b.x, b.y, b.w, b.h, 6, COLOR_WHITE);
    gfx->setTextSize(1);
    gfx->setTextColor(fg);
    gfx->setCursor(b.x + 16, b.y + 13);
    gfx->println(b.label);
  }
}

void drawHeaderStatus()
{
  updateBatteryStatus();

  gfx->fillRect(380, 2, 96, 30, COLOR_BLACK);
  gfx->setTextColor(COLOR_WHITE);
  gfx->setTextSize(1);
  gfx->setCursor(382, 6);
  gfx->printf("FPS:%u", kFpsOptions[g_settings.fpsIndex]);

  gfx->setCursor(382, 18);
  if (g_batteryPresent)
  {
    gfx->printf("BAT:%u%%", g_batteryPercent);
  }
  else
  {
    gfx->println("BAT:N/A");
  }
}

void clearSceneArea()
{
  gfx->fillRect(0, kSceneTop, kScreenWidth, kSceneH, COLOR_BLACK);
}

void drawScenePalette()
{
  clearSceneArea();
  const uint16_t c[8] = {COLOR_RED, COLOR_GREEN, COLOR_BLUE, COLOR_YELLOW, COLOR_CYAN, COLOR_MAGENTA, COLOR_WHITE, COLOR_ORANGE};
  int w = kScreenWidth / 4;
  int h = kSceneH / 2;

  for (int i = 0; i < 8; i++)
  {
    int x = (i % 4) * w;
    int y = kSceneTop + (i / 4) * h;
    gfx->fillRect(x, y, w, h, c[i]);
    gfx->drawRect(x, y, w, h, COLOR_BLACK);
  }
}

void drawSceneShapes()
{
  clearSceneArea();
  gfx->drawRect(12, kSceneTop + 10, 90, 70, COLOR_RED);
  gfx->fillRect(112, kSceneTop + 10, 90, 70, COLOR_GREEN);
  gfx->drawCircle(250, kSceneTop + 45, 30, COLOR_BLUE);
  gfx->fillCircle(340, kSceneTop + 45, 30, COLOR_YELLOW);
  gfx->drawLine(12, kSceneTop + 100, 220, kSceneTop + 100, COLOR_CYAN);
  gfx->drawLine(12, kSceneTop + 120, 220, kSceneTop + 140, COLOR_MAGENTA);
  gfx->setTextColor(COLOR_WHITE);
  gfx->setTextSize(2);
  gfx->setCursor(250, kSceneTop + 100);
  gfx->println("Shapes");
}

void drawSceneBars(bool forceFullRedraw)
{
  const uint16_t colors[kBarsCount] = {COLOR_RED, COLOR_GREEN, COLOR_BLUE, COLOR_YELLOW};
  int regionTop = kSceneTop + 10;
  int regionBottom = kSceneBottom - 10;
  int regionH = regionBottom - regionTop;
  int barW = kScreenWidth / kBarsCount;
  int margin = 14;

  if (forceFullRedraw)
  {
    clearSceneArea();
    gfx->drawRect(0, regionTop, kScreenWidth, regionH, COLOR_WHITE);
    resetBarsState();
  }

  for (int i = 0; i < kBarsCount; i++)
  {
    float phase = (2.0f * PI * (g_uiFrame + (i * 11))) / 48.0f;
    int height = 24 + static_cast<int>((regionH - 28) * (0.5f + 0.5f * cosf(phase)));
    int x = (i * barW) + margin;
    int y = regionBottom - height;
    int innerW = barW - (2 * margin);

    int prevTop = g_prevBarTop[i];
    int prevH = g_prevBarHeight[i];
    int prevBottom = prevTop + prevH;
    int newBottom = y + height;

    if (prevH == 0)
    {
      gfx->fillRect(x, y, innerW, height, colors[i]);
    }
    else
    {
      if (y < prevTop)
      {
        gfx->fillRect(x, y, innerW, prevTop - y, colors[i]);
      }
      else if (y > prevTop)
      {
        gfx->fillRect(x, prevTop, innerW, y - prevTop, COLOR_BLACK);
      }

      if (newBottom > prevBottom)
      {
        gfx->fillRect(x, prevBottom, innerW, newBottom - prevBottom, colors[i]);
      }
      else if (newBottom < prevBottom)
      {
        gfx->fillRect(x, newBottom, innerW, prevBottom - newBottom, COLOR_BLACK);
      }
    }

    g_prevBarTop[i] = y;
    g_prevBarHeight[i] = height;
  }
}

void drawSceneGradient()
{
  clearSceneArea();
  for (int x = 0; x < kScreenWidth; x += 2)
  {
    uint16_t r = (x * 31) / kScreenWidth;
    uint16_t g = ((kScreenWidth - x) * 63) / kScreenWidth;
    uint16_t b = 15;
    uint16_t color = (r << 11) | (g << 5) | b;
    gfx->drawLine(x, kSceneTop, x, kSceneBottom - 1, color);
  }
}

void drawSettingsScene(bool forceRedraw)
{
  if (!forceRedraw)
  {
    return;
  }

  clearSceneArea();
  gfx->setTextColor(COLOR_WHITE);
  gfx->setTextSize(2);
  gfx->setCursor(14, kSceneTop + 10);
  gfx->println("Impostazioni");

  gfx->setTextSize(1);
  gfx->setCursor(20, kSceneTop + 44);
  gfx->printf("Luminosita: %u%%", g_settings.brightnessPct);
  gfx->drawRoundRect(260, kSceneTop + 36, 28, 24, 4, COLOR_WHITE);
  gfx->drawRoundRect(296, kSceneTop + 36, 28, 24, 4, COLOR_WHITE);
  gfx->setCursor(270, kSceneTop + 45);
  gfx->println("-");
  gfx->setCursor(307, kSceneTop + 45);
  gfx->println("+");

  gfx->setCursor(20, kSceneTop + 80);
  gfx->printf("FPS: %u", kFpsOptions[g_settings.fpsIndex]);
  gfx->drawRoundRect(260, kSceneTop + 72, 88, 24, 4, COLOR_WHITE);
  gfx->setCursor(272, kSceneTop + 81);
  gfx->println("Cambia FPS");

  if (g_batteryPresent)
  {
    gfx->setCursor(20, kSceneTop + 116);
    gfx->printf("Batteria: %u%% (%.2fV)", g_batteryPercent, g_batteryVoltage);
    gfx->drawRoundRect(260, kSceneTop + 108, 146, 24, 4, COLOR_WHITE);
    gfx->setCursor(270, kSceneTop + 117);
    gfx->printf("Risparmio: %s", g_settings.powerSaveEnabled ? "ON" : "OFF");
  }
  else
  {
    gfx->setCursor(20, kSceneTop + 116);
    gfx->println("Batteria: non rilevata / monitor non configurato");
  }

  gfx->drawRoundRect(20, kSceneTop + 156, 190, 28, 4, COLOR_WHITE);
  gfx->setCursor(34, kSceneTop + 166);
  gfx->println("Rifai calibrazione touch");

  gfx->setCursor(20, kSceneTop + 198);
  if (BATTERY_ADC_PIN >= 0)
  {
    gfx->printf("BAT pin ADC: %d", BATTERY_ADC_PIN);
  }
  else
  {
    gfx->println("BAT pin ADC: non configurato");
  }
}

void renderScene(bool forceFullRedraw)
{
  switch (g_scene)
  {
  case SCENE_PALETTE:
    if (forceFullRedraw) drawScenePalette();
    break;
  case SCENE_SHAPES:
    if (forceFullRedraw) drawSceneShapes();
    break;
  case SCENE_BARS:
    drawSceneBars(forceFullRedraw);
    break;
  case SCENE_GRADIENT:
    if (forceFullRedraw) drawSceneGradient();
    break;
  case SCENE_SETTINGS:
    drawSettingsScene(forceFullRedraw);
    break;
  }
}

int hitTestButton(int16_t x, int16_t y)
{
  for (uint8_t i = 0; i < 5; i++)
  {
    UiButton &b = g_buttons[i];
    if (x >= b.x && x < (b.x + b.w) && y >= b.y && y < (b.y + b.h))
    {
      return i;
    }
  }
  return -1;
}

void enterCalibrationMode(bool requireRelease)
{
  g_mode = TOUCH_MODE_CALIBRATION;
  g_calStep = 0;
  g_calPressActive = false;
  g_waitCalibrationRelease = requireRelease;
  g_calXSum = 0;
  g_calYSum = 0;
  g_calSamples = 0;
  drawCalibrationScreen();
}

void finishCalibration()
{
  int32_t rawLeftTarget = (g_calRawX[0] + g_calRawX[3]) / 2;
  int32_t rawRightTarget = (g_calRawX[1] + g_calRawX[2]) / 2;
  int32_t rawTopTarget = (g_calRawY[0] + g_calRawY[1]) / 2;
  int32_t rawBottomTarget = (g_calRawY[2] + g_calRawY[3]) / 2;

  g_rawEdgeLeft = extrapolateRawEdge(rawLeftTarget, rawRightTarget, g_targets[0].x, g_targets[1].x, 0);
  g_rawEdgeRight = extrapolateRawEdge(rawLeftTarget, rawRightTarget, g_targets[0].x, g_targets[1].x, kScreenWidth - 1);
  g_rawEdgeTop = extrapolateRawEdge(rawTopTarget, rawBottomTarget, g_targets[0].y, g_targets[3].y, 0);
  g_rawEdgeBottom = extrapolateRawEdge(rawTopTarget, rawBottomTarget, g_targets[0].y, g_targets[3].y, kScreenHeight - 1);

  saveCalibration();

  Serial.printf("Calibration saved: edgeLeft=%ld edgeRight=%ld edgeTop=%ld edgeBottom=%ld\n",
    g_rawEdgeLeft, g_rawEdgeRight, g_rawEdgeTop, g_rawEdgeBottom);

  g_mode = TOUCH_MODE_UI;
  g_scene = SCENE_PALETTE;
  drawUiChrome();
  drawButtons();
  g_sceneNeedsRedraw = true;
  g_uiFrame = 0;
}

void handleSettingsTouch(uint16_t x, uint16_t y)
{
  // Brightness -
  if (x >= 260 && x < 288 && y >= (kSceneTop + 36) && y < (kSceneTop + 60))
  {
    if (g_settings.brightnessPct >= 5)
    {
      g_settings.brightnessPct -= 5;
    }
    applyBacklightBrightness();
    saveUiSettings();
    g_sceneNeedsRedraw = true;
    return;
  }

  // Brightness +
  if (x >= 296 && x < 324 && y >= (kSceneTop + 36) && y < (kSceneTop + 60))
  {
    if (g_settings.brightnessPct <= 95)
    {
      g_settings.brightnessPct += 5;
    }
    applyBacklightBrightness();
    saveUiSettings();
    g_sceneNeedsRedraw = true;
    return;
  }

  // FPS cycle
  if (x >= 260 && x < 348 && y >= (kSceneTop + 72) && y < (kSceneTop + 96))
  {
    g_settings.fpsIndex = (g_settings.fpsIndex + 1) % kFpsOptionsCount;
    saveUiSettings();
    g_sceneNeedsRedraw = true;
    return;
  }

  // Power save toggle (only if battery present)
  if (g_batteryPresent && x >= 260 && x < 406 && y >= (kSceneTop + 108) && y < (kSceneTop + 132))
  {
    g_settings.powerSaveEnabled = !g_settings.powerSaveEnabled;
    applyBacklightBrightness();
    saveUiSettings();
    g_sceneNeedsRedraw = true;
    return;
  }

  // Recalibrate button
  if (x >= 20 && x < 210 && y >= (kSceneTop + 156) && y < (kSceneTop + 184))
  {
    enterCalibrationMode(true);
    return;
  }
}

void setup()
{
  Serial.begin(115200);
  delay(500);

  Serial.println("\n=== CrowPanel Step 4: Touch UI + Settings ===");
  Serial.printf("Chip model: %s\n", ESP.getChipModel());
  Serial.printf("Flash: %u MB, PSRAM: %u MB\n", ESP.getFlashChipSize() / (1024 * 1024), ESP.getPsramSize() / (1024 * 1024));

  pinMode(2, OUTPUT);
  pinMode(38, OUTPUT);
  digitalWrite(38, HIGH);
  
  // Check GPIO37 boot control pin with double-read for stability
  pinMode(GPIO_BOOT_CONTROL, INPUT_PULLDOWN);
  delay(200);
  bool bootRead1 = (digitalRead(GPIO_BOOT_CONTROL) == HIGH);
  delay(100);
  bool bootRead2 = (digitalRead(GPIO_BOOT_CONTROL) == HIGH);
  bool bootControlActive = (bootRead1 && bootRead2);
  
  if (bootControlActive)
  {
    Serial.printf(">>> BOOT CONTROL DETECTED: GPIO%d is HIGH <<<\n", GPIO_BOOT_CONTROL);
    Serial.println("Forcing factory reset and calibration mode");
    resetToFactoryDefaults();
    delay(200);
  }
  else
  {
    Serial.println("GPIO37 normal state, loading saved settings");
  }

  // Load settings from NVS (will use defaults if not found or after reset)
  loadUiSettings();
  delay(100);
  
  // Apply brightness BEFORE display init
  applyBacklightBrightness();
  Serial.printf("Brightness applied: %d%%, FPS: %d\n", g_settings.brightnessPct, kFpsOptions[g_settings.fpsIndex]);

  if (BATTERY_ADC_PIN >= 0)
  {
    pinMode(BATTERY_ADC_PIN, INPUT);
#if defined(ESP32)
    analogSetPinAttenuation(BATTERY_ADC_PIN, ADC_11db);
#endif
  }

  if (!gfx->begin())
  {
    Serial.println("ERROR: Display init failed");
    while (true) delay(1000);
  }

  drawWelcomeScreen();
  delay(1700);

  touchSpi.begin(12, 13, 11, 0);
  touch.begin(touchSpi);
  touch.setRotation(0);

  if (bootControlActive || !loadCalibration())
  {
    if (bootControlActive)
    {
      Serial.println(">>> Starting calibration flow due to boot control pin <<<");
    }
    else
    {
      Serial.println("No valid calibration in NVS, starting calibration flow");
    }
    enterCalibrationMode(false);
  }
  else
  {
    Serial.println("Calibration loaded from NVS");
    g_mode = TOUCH_MODE_UI;
    drawUiChrome();
    drawButtons();
    g_sceneNeedsRedraw = true;
  }

  g_uiInit = true;
  g_lastFrameMs = millis();
  Serial.println("=== Setup complete, UI initialized ===\n");
}

void loop()
{
  if (!g_uiInit)
  {
    delay(10);
    return;
  }

  unsigned long now = millis();
  unsigned long frameInterval = getFrameIntervalMs();
  if (now - g_lastFrameMs < frameInterval)
  {
    delay(1);
    return;
  }
  g_lastFrameMs = now;

  uint16_t rawX = 0;
  uint16_t rawY = 0;
  uint16_t pressure = 0;
  bool pressed = sampleTouch(rawX, rawY, pressure);

  if (g_mode == TOUCH_MODE_CALIBRATION)
  {
    if (g_waitCalibrationRelease)
    {
      if (!pressed)
      {
        g_waitCalibrationRelease = false;
        gfx->fillRect(16, 246, 448, 14, COLOR_BLACK);
        gfx->setTextColor(COLOR_GREEN);
        gfx->setTextSize(1);
        gfx->setCursor(20, 248);
        gfx->println("OK: ora tocca il primo target");
      }
      delay(1);
      return;
    }

    if (pressed)
    {
      g_calPressActive = true;
      g_calXSum += rawX;
      g_calYSum += rawY;
      g_calSamples++;
    }
    else if (g_calPressActive)
    {
      if (g_calSamples > 0)
      {
        g_calRawX[g_calStep] = static_cast<uint16_t>(g_calXSum / g_calSamples);
        g_calRawY[g_calStep] = static_cast<uint16_t>(g_calYSum / g_calSamples);
        Serial.printf("cal step=%u %s raw=(%u,%u) samples=%u\n",
          g_calStep + 1,
          g_targets[g_calStep].label,
          g_calRawX[g_calStep],
          g_calRawY[g_calStep],
          g_calSamples);

        g_calStep++;
        if (g_calStep >= 4)
        {
          finishCalibration();
        }
        else
        {
          drawCalibrationScreen();
        }
      }

      g_calPressActive = false;
      g_calXSum = 0;
      g_calYSum = 0;
      g_calSamples = 0;
    }

    delay(1);
    return;
  }

  g_uiFrame++;
  updateBatteryStatus();
  drawHeaderStatus();

  uint16_t x = mapTouchX(rawX);
  uint16_t y = mapTouchY(rawY);

  // Stabilize mapped coordinates when the finger is held on a fixed point.
  if (pressed)
  {
    if (!g_hasFilteredTouch)
    {
      g_filteredX = static_cast<int16_t>(x);
      g_filteredY = static_cast<int16_t>(y);
      g_hasFilteredTouch = true;
    }
    else
    {
      int16_t targetX = static_cast<int16_t>(x);
      int16_t targetY = static_cast<int16_t>(y);
      int16_t nx = static_cast<int16_t>((g_filteredX * 3 + targetX) / 4);
      int16_t ny = static_cast<int16_t>((g_filteredY * 3 + targetY) / 4);

      if (abs(nx - g_filteredX) <= 1)
      {
        nx = g_filteredX;
      }
      if (abs(ny - g_filteredY) <= 1)
      {
        ny = g_filteredY;
      }

      g_filteredX = nx;
      g_filteredY = ny;
    }

    x = static_cast<uint16_t>(g_filteredX);
    y = static_cast<uint16_t>(g_filteredY);
  }
  else
  {
    g_hasFilteredTouch = false;
  }

  if (pressed && !g_lastPressed)
  {
    int btn = hitTestButton(static_cast<int16_t>(x), static_cast<int16_t>(y));
    if (btn >= 0)
    {
      DemoScene next = g_buttons[btn].scene;
      if (next != g_scene)
      {
        g_scene = next;
        g_sceneNeedsRedraw = true;
        drawButtons();
        Serial.printf("scene=%d\n", static_cast<int>(g_scene));
      }
    }
    else if (g_scene == SCENE_SETTINGS)
    {
      handleSettingsTouch(x, y);
    }
  }

  renderScene(g_sceneNeedsRedraw);
  g_sceneNeedsRedraw = false;

  // Pointer feedback only in non-settings visual demos.
  if (g_scene != SCENE_SETTINGS)
  {
    if (g_lastPressed && (!pressed || x != g_lastX || y != g_lastY))
    {
      if (g_lastY >= kSceneTop && g_lastY < kSceneBottom)
      {
        gfx->fillCircle(g_lastX, g_lastY, 4, COLOR_BLACK);
      }
    }

    if (pressed && y >= kSceneTop && y < kSceneBottom)
    {
      gfx->fillCircle(x, y, 4, COLOR_WHITE);
    }
  }

  g_lastPressed = pressed;
  g_lastX = static_cast<int16_t>(x);
  g_lastY = static_cast<int16_t>(y);

  if (pressed && now - g_lastTouchLogMs >= 160)
  {
    Serial.printf("touch raw=(%u,%u,%u) cal=(%u,%u) scene=%d\n", rawX, rawY, pressure, x, y, static_cast<int>(g_scene));
    g_lastTouchLogMs = now;
  }

  delay(1);
}
