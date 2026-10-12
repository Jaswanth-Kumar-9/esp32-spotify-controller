#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <U8g2lib.h>
#include <time.h>

// ======================================================
// PUBLIC-SAFE LOCAL CONFIGURATION
//
// Keep these as placeholders in a public repository. The matching variable
// names are documented in .env.example. Before flashing your own device,
// replace the placeholder values locally and do not commit the changes.
// ======================================================

const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

const char* BRIDGE_HOST = "YOUR_LAPTOP_LAN_IP";
const uint16_t BRIDGE_PORT = 8888;

const char* API_KEY = "YOUR_BRIDGE_API_KEY";

// ======================================================
// WIDGET BUTTON / WEATHER

// Wire the push button between this pin and GND.
// INPUT_PULLUP means the button reads LOW when pressed.
const int WIDGET_BUTTON_PIN = 27;

// Change these to the location whose weather you want.
const float WEATHER_LATITUDE = 17.3850;   // Hyderabad example
const float WEATHER_LONGITUDE = 78.4867;
const char* WEATHER_LOCATION = "Hyderabad";

// ======================================================
// OLED
// SH1106 128x64 SPI
//
// SCL -> GPIO 18 (SPI clock)
// SDA -> GPIO 23 (SPI MOSI)
// RES -> GPIO 25
// DC  -> GPIO 26
//
// This is the exact constructor used successfully
// with your display.
// ======================================================

U8G2_SH1106_128X64_NONAME_F_4W_HW_SPI display(
  U8G2_R0,
  U8X8_PIN_NONE,
  26,
  25
);

// ======================================================
// TIMING
// ======================================================

const unsigned long PLAYER_POLL_MS = 200;
const unsigned long TEXT_SCROLL_MS = 45;
const unsigned long DEVICE_INFO_MS = 1800;
const unsigned long WIFI_RETRY_MS = 5000;
const unsigned long BUTTON_DEBOUNCE_MS = 40;
const unsigned long WEATHER_REFRESH_MS = 10UL * 60UL * 1000UL;

// ======================================================
// PLAYER STATE
// ======================================================

String title = "";
String artist = "";
String deviceName = "";

bool playing = false;

unsigned long progressMs = 0;
unsigned long durationMs = 0;

unsigned long lastPlayerPoll = 0;
unsigned long lastWifiAttempt = 0;
unsigned long lastWeatherRefresh = 0;

// ======================================================
// WIDGET STATE
// ======================================================

enum WidgetPage
{
  WIDGET_PLAYER,
  WIDGET_CLOCK,
  WIDGET_WEATHER
};

WidgetPage currentWidget = WIDGET_PLAYER;
bool lastButtonReading = HIGH;
bool stableButtonState = HIGH;
unsigned long lastButtonChange = 0;

float weatherTemperature = 0;
int weatherCode = -1;
int weatherHumidity = 0;
bool weatherValid = false;
String weatherError = "";

// Pixel scroll state
int titleScrollPx = 0;
int artistScrollPx = 0;
int deviceScrollPx = 0;

unsigned long lastTitleScroll = 0;
unsigned long lastArtistScroll = 0;
unsigned long lastDeviceScroll = 0;

// Temporary Spotify device info screen
unsigned long deviceInfoUntil = 0;

// ======================================================
// DISPLAY STATE
// ======================================================

enum DisplayStatus
{
  DISPLAY_OK,
  DISPLAY_BRIDGE_OFFLINE,
  DISPLAY_BRIDGE_DENIED,
  DISPLAY_SPOTIFY_ERROR,
  DISPLAY_BAD_JSON
};

DisplayStatus displayStatus = DISPLAY_OK;
int lastPlayerErrorCode = 0;

// ======================================================
// TEXT HELPERS
// ======================================================

String cleanText(String s)
{
  s.replace("\n", " ");
  s.replace("\r", " ");
  return s;
}

String formatTime(unsigned long ms)
{
  unsigned long totalSeconds = ms / 1000;
  unsigned long minutes = totalSeconds / 60;
  unsigned long seconds = totalSeconds % 60;

  String result = String(minutes) + ":";

  if (seconds < 10)
    result += "0";

  result += String(seconds);

  return result;
}

void drawCentered(const String& text, int y)
{
  int width = display.getStrWidth(text.c_str());
  int x = (128 - width) / 2;

  if (x < 0)
    x = 0;

  display.drawStr(x, y, text.c_str());
}

String weatherDescription(int code)
{
  if (code == 0) return "Clear sky";
  if (code <= 3) return "Partly cloudy";
  if (code <= 48) return "Foggy";
  if (code <= 67) return "Rain";
  if (code <= 77) return "Snow";
  if (code <= 82) return "Showers";
  if (code <= 99) return "Thunderstorm";
  return "Unknown";
}

void showClockScreen()
{
  struct tm timeInfo;

  if (!getLocalTime(&timeInfo, 50))
  {
    showStatusScreen("TIME UNAVAILABLE", "Waiting for NTP", false);
    return;
  }

  char timeText[9];
  char dateText[16];
  strftime(timeText, sizeof(timeText), "%H:%M:%S", &timeInfo);
  strftime(dateText, sizeof(dateText), "%d/%m/%Y", &timeInfo);

  display.clearBuffer();
  display.setFont(u8g2_font_6x10_tf);
  drawCentered("CURRENT TIME", 12);
  display.drawHLine(20, 17, 88);
  display.setFont(u8g2_font_ncenB14_tr);
  drawCentered(timeText, 39);
  display.setFont(u8g2_font_7x14B_tf);
  drawCentered(dateText, 58);
  display.sendBuffer();
}

void showWeatherScreen()
{
  display.clearBuffer();
  display.setFont(u8g2_font_6x10_tf);
  drawCentered("CURRENT WEATHER", 11);
  display.drawHLine(12, 16, 104);

  if (!weatherValid)
  {
    display.setFont(u8g2_font_6x10_tf);
    drawCentered(weatherError.length() ? weatherError : "Waiting for weather", 35);
    display.sendBuffer();
    return;
  }

  String temperature = String(weatherTemperature, 1) + " C";
  display.setFont(u8g2_font_ncenB14_tr);
  drawCentered(temperature, 36);

  display.setFont(u8g2_font_6x10_tf);
  drawCentered(weatherDescription(weatherCode), 48);
  drawCentered(String(WEATHER_LOCATION) + "  " + String(weatherHumidity) + "% RH", 61);
  display.sendBuffer();
}

// ======================================================
// SCROLLING TEXT
//
// Uses U8g2's clipping window so long titles/artists do
// not overwrite the rest of the UI.
// ======================================================

void drawScrollingText(
  const String& text,
  int x,
  int y,
  int areaWidth,
  int& scrollPx,
  unsigned long& lastScroll,
  int gapPx = 24
)
{
  if (text.length() == 0)
    return;

  int textWidth = display.getStrWidth(text.c_str());

  // Text fits: draw normally and reset the scroll position.
  if (textWidth <= areaWidth)
  {
    display.drawStr(x, y, text.c_str());
    scrollPx = 0;
    return;
  }

  const int cycleWidth = textWidth + gapPx;

  if (millis() - lastScroll >= TEXT_SCROLL_MS)
  {
    lastScroll = millis();
    scrollPx++;

    if (scrollPx >= cycleWidth)
      scrollPx = 0;
  }

  // U8g2 clipping window uses x1/y1 as the exclusive end.
  display.setClipWindow(
    x,
    0,
    x + areaWidth,
    64
  );

  int drawX = x - scrollPx;

  display.drawStr(drawX, y, text.c_str());
  display.drawStr(drawX + cycleWidth, y, text.c_str());

  display.setMaxClipWindow();
}

// ======================================================
// DISPLAY SCREENS
// ======================================================

void showStartupScreen()
{
  display.clearBuffer();

  display.setFont(u8g2_font_ncenB14_tr);
  drawCentered("SPOTIFY", 24);

  display.setFont(u8g2_font_6x10_tf);
  drawCentered("ESP32 PLAYER", 41);

  display.drawHLine(28, 49, 72);

  display.sendBuffer();
}

void showStatusScreen(
  const String& line1,
  const String& line2,
  bool divider
)
{
  display.clearBuffer();

  display.setFont(u8g2_font_ncenB10_tr);
  drawCentered(line1, 24);

  display.setFont(u8g2_font_6x10_tf);
  drawCentered(line2, 41);

  if (divider)
    display.drawHLine(20, 50, 88);

  display.sendBuffer();
}

void showDeviceScreen()
{
  display.clearBuffer();

  display.setFont(u8g2_font_6x10_tf);
  drawCentered("PLAYING ON", 12);

  display.drawHLine(20, 17, 88);

  display.setFont(u8g2_font_7x14B_tf);

  drawScrollingText(
    deviceName,
    4,
    37,
    120,
    deviceScrollPx,
    lastDeviceScroll
  );

  display.setFont(u8g2_font_6x10_tf);
  drawCentered("Spotify device", 56);

  display.sendBuffer();
}

void showNoMusicScreen()
{
  display.clearBuffer();

  display.setFont(u8g2_font_ncenB14_tr);
  drawCentered("NO MUSIC", 25);

  display.setFont(u8g2_font_6x10_tf);
  drawCentered("Open Spotify", 43);

  display.sendBuffer();
}

// ======================================================
// MAIN PLAYER UI
//
// Layout:
//  0-16 : song title
// 18-28 : artist
// 32-40 : progress bar
// 47-59 : timestamps / pause icon
// ======================================================

void updateDisplay()
{
  // Widget pages take priority over the Spotify player page.
  if (currentWidget == WIDGET_CLOCK)
  {
    showClockScreen();
    return;
  }

  if (currentWidget == WIDGET_WEATHER)
  {
    showWeatherScreen();
    return;
  }

  // ----------------------------------------------------
  // Temporary device screen
  // ----------------------------------------------------

  if (
    deviceInfoUntil > millis() &&
    deviceName.length() > 0 &&
    displayStatus == DISPLAY_OK
  )
  {
    showDeviceScreen();
    return;
  }

  // ----------------------------------------------------
  // Error / status screens
  // ----------------------------------------------------

  if (displayStatus == DISPLAY_BRIDGE_OFFLINE)
  {
    showStatusScreen("BRIDGE OFFLINE", "Check laptop", false);
    return;
  }

  if (displayStatus == DISPLAY_BRIDGE_DENIED)
  {
    showStatusScreen("BRIDGE DENIED", "Check API key", false);
    return;
  }

  if (displayStatus == DISPLAY_SPOTIFY_ERROR)
  {
    showStatusScreen("SPOTIFY ERROR", "HTTP " + String(lastPlayerErrorCode), false);
    return;
  }

  if (displayStatus == DISPLAY_BAD_JSON)
  {
    showStatusScreen("BAD JSON", "Bridge response", false);
    return;
  }

  // ----------------------------------------------------
  // No current track
  // ----------------------------------------------------

  if (title.length() == 0)
  {
    showNoMusicScreen();
    return;
  }

  // ----------------------------------------------------
  // Main player screen
  // ----------------------------------------------------

  display.clearBuffer();

  // Song title
  display.setFont(u8g2_font_7x14B_tf);
  drawScrollingText(
    title,
    4,
    14,
    120,
    titleScrollPx,
    lastTitleScroll
  );

  // Artist
  display.setFont(u8g2_font_6x10_tf);
  drawScrollingText(
    artist,
    4,
    27,
    120,
    artistScrollPx,
    lastArtistScroll
  );

  // Separator -- safely below artist baseline
  display.drawHLine(4, 30, 120);

  // ----------------------------------------------------
  // Progress bar
  // ----------------------------------------------------

  const int barX = 4;
  const int barY = 34;
  const int barW = 120;
  const int barH = 8;

  display.drawFrame(barX, barY, barW, barH);

  if (durationMs > 0)
  {
    unsigned long safeProgress = progressMs;

    if (safeProgress > durationMs)
      safeProgress = durationMs;

    const int innerW = barW - 4;

    int fillW = (int)(
      ((uint64_t)safeProgress * innerW) /
      durationMs
    );

    if (fillW > 0)
    {
      display.drawBox(
        barX + 2,
        barY + 2,
        fillW,
        barH - 4
      );
    }
  }

  // ----------------------------------------------------
  // Timestamps
  // ----------------------------------------------------

  display.setFont(u8g2_font_6x10_tf);

  String currentTime = formatTime(progressMs);
  String totalTime = formatTime(durationMs);

  display.drawStr(4, 56, currentTime.c_str());
  display.drawStr(94, 56, totalTime.c_str());

  // ----------------------------------------------------
  // Pause icon ONLY when paused.
  // Nothing is drawn when playing.
  // ----------------------------------------------------

  if (!playing)
  {
    const int iconX = 60;
    const int iconY = 47;
    const int iconW = 4;
    const int iconH = 10;

    display.drawBox(iconX, iconY, iconW, iconH);
    display.drawBox(iconX + 7, iconY, iconW, iconH);
  }

  display.sendBuffer();
}

void updateWeather()
{
  String path = "/v1/forecast?latitude=" + String(WEATHER_LATITUDE, 4) +
                "&longitude=" + String(WEATHER_LONGITUDE, 4) +
                "&current=temperature_2m,relative_humidity_2m,weather_code" +
                "&timezone=auto";

  String response;
  int code = -1;

  if (WiFi.status() == WL_CONNECTED)
  {
    WiFiClientSecure secureClient;
    secureClient.setInsecure(); // API endpoint is public; no certificate storage needed.

    HTTPClient http;
    String url = "https://api.open-meteo.com" + path;

    if (http.begin(secureClient, url))
    {
      code = http.GET();
      response = http.getString();
      http.end();
    }
  }

  if (code != 200)
  {
    weatherValid = false;
    weatherError = "Weather offline";
    lastWeatherRefresh = millis();
    return;
  }

  JsonDocument doc;
  if (deserializeJson(doc, response))
  {
    weatherValid = false;
    weatherError = "Weather error";
    lastWeatherRefresh = millis();
    return;
  }

  JsonObject current = doc["current"];
  if (current.isNull())
  {
    weatherValid = false;
    weatherError = "No weather data";
    lastWeatherRefresh = millis();
    return;
  }

  weatherTemperature = current["temperature_2m"] | 0.0;
  weatherHumidity = current["relative_humidity_2m"] | 0;
  weatherCode = current["weather_code"] | -1;
  weatherValid = true;
  weatherError = "";
  lastWeatherRefresh = millis();
}

void handleWidgetButton()
{
  bool reading = digitalRead(WIDGET_BUTTON_PIN);

  if (reading != lastButtonReading)
    lastButtonChange = millis();

  if (millis() - lastButtonChange > BUTTON_DEBOUNCE_MS)
  {
    if (reading != stableButtonState)
    {
      stableButtonState = reading;

      // Advance only when the button is pressed, not released.
      if (stableButtonState == LOW)
      {
        currentWidget = (WidgetPage)((currentWidget + 1) % 3);

        if (currentWidget == WIDGET_WEATHER &&
            (!weatherValid || millis() - lastWeatherRefresh >= WEATHER_REFRESH_MS))
        {
          updateWeather();
        }
      }
    }
  }

  lastButtonReading = reading;
}

// ======================================================
// WIFI
// ======================================================

void connectWiFi()
{
  if (WiFi.status() == WL_CONNECTED)
    return;

  showStatusScreen("WIFI CONNECTING", "Please wait...", true);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("Connecting to WiFi");

  unsigned long start = millis();

  while (
    WiFi.status() != WL_CONNECTED &&
    millis() - start < 15000
  )
  {
    delay(300);
    Serial.print(".");
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED)
  {
    Serial.print("ESP32 IP: ");
    Serial.println(WiFi.localIP());

    showStatusScreen("WIFI CONNECTED", WiFi.localIP().toString(), false);
    delay(1000);
  }
  else
  {
    showStatusScreen("WIFI FAILED", "Retrying...", false);
    delay(1000);
  }
}

// ======================================================
// HTTP HELPER
//
// Kept compatible with the existing bridge/API.
// Only GET is used now because button command functions
// have been removed.
// ======================================================

int httpRequest(
  const char* method,
  const String& path,
  String& response
)
{
  if (WiFi.status() != WL_CONNECTED)
    return -1;

  HTTPClient http;

  String url =
    "http://" +
    String(BRIDGE_HOST) +
    ":" +
    String(BRIDGE_PORT) +
    path;

  if (!http.begin(url))
    return -1;

  if (strlen(API_KEY) > 0)
    http.addHeader("X-API-Key", API_KEY);

  int code = -1;

  if (strcmp(method, "GET") == 0)
  {
    code = http.GET();
  }
  else
  {
    code = http.POST("");
  }

  response = http.getString();
  http.end();

  return code;
}

// ======================================================
// PLAYER STATUS
// ======================================================

void updatePlayer()
{
  String response;

  int code = httpRequest(
    "GET",
    "/api/player",
    response
  );

  Serial.print("Player HTTP: ");
  Serial.println(code);

  if (code == 401)
  {
    lastPlayerErrorCode = code;
    displayStatus = DISPLAY_BRIDGE_DENIED;
    return;
  }

  if (code < 0)
  {
    lastPlayerErrorCode = code;
    displayStatus = DISPLAY_BRIDGE_OFFLINE;
    return;
  }

  if (code != 200)
  {
    lastPlayerErrorCode = code;
    displayStatus = DISPLAY_SPOTIFY_ERROR;
    return;
  }

  JsonDocument doc;

  DeserializationError error =
    deserializeJson(doc, response);

  if (error)
  {
    Serial.print("JSON error: ");
    Serial.println(error.c_str());

    displayStatus = DISPLAY_BAD_JSON;
    return;
  }

  bool newPlaying =
    doc["playing"] | false;

  String newTitle =
    doc["title"] | "";

  String newArtist =
    doc["artist"] | "";

  String newDevice =
    doc["device"] | "";

  unsigned long newProgress =
    doc["progress_ms"] | 0;

  unsigned long newDuration =
    doc["duration_ms"] | 0;

  // Reset title/artist scrolling when track changes.
  if (newTitle != title)
  {
    titleScrollPx = 0;
    artistScrollPx = 0;
    lastTitleScroll = millis();
    lastArtistScroll = millis();
  }

  // Show device screen when the Spotify device changes.
  if (
    newDevice.length() > 0 &&
    newDevice != deviceName
  )
  {
    deviceInfoUntil = millis() + DEVICE_INFO_MS;
    deviceScrollPx = 0;
    lastDeviceScroll = millis();
  }

  playing = newPlaying;
  title = cleanText(newTitle);
  artist = cleanText(newArtist);
  deviceName = cleanText(newDevice);

  progressMs = newProgress;
  durationMs = newDuration;

  // Successful response clears old error status.
  displayStatus = DISPLAY_OK;
}

// ======================================================
// SETUP
// ======================================================

void setup()
{
  Serial.begin(115200);

  // OLED uses hardware SPI on the ESP32's default VSPI pins:
  // SCL -> GPIO18
  // SDA -> GPIO23
  // DC  -> GPIO26
  // RES -> GPIO25
  display.begin();
  display.setContrast(255);
  display.setFontPosBaseline();

  pinMode(WIDGET_BUTTON_PIN, INPUT_PULLUP);

  // India Standard Time: UTC+5:30, no daylight saving time.
  configTime(19800, 0, "pool.ntp.org", "time.nist.gov");

  // ----------------------------------------------------
  // Startup screen
  // ----------------------------------------------------

  showStartupScreen();
  delay(1400);

  connectWiFi();

  showStatusScreen(
    "SPOTIFY BRIDGE",
    "Connecting...",
    true
  );

  delay(600);

  updatePlayer();
  updateWeather();
}

// ======================================================
// LOOP
// ======================================================

void loop()
{
  handleWidgetButton();

  // Keep WiFi alive.
  if (WiFi.status() != WL_CONNECTED)
  {
    if (millis() - lastWifiAttempt >= WIFI_RETRY_MS)
    {
      lastWifiAttempt = millis();
      connectWiFi();
    }
  }

  // Poll Spotify state.
  if (millis() - lastPlayerPoll >= PLAYER_POLL_MS)
  {
    lastPlayerPoll = millis();
    updatePlayer();
  }

  if (WiFi.status() == WL_CONNECTED &&
      millis() - lastWeatherRefresh >= WEATHER_REFRESH_MS)
  {
    updateWeather();
  }

  // Refresh OLED.
  updateDisplay();

  delay(10);
}


