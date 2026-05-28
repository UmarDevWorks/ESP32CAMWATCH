#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <esp_camera.h>
#include <time.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "esp_sleep.h"
#include "FS.h"
#include "SD_MMC.h"

/* ================= ESP32-CAM SAFE PIN MAP ================= */
// OLED (I2C)
#define SDA_PIN 3 // GPIO 3 (U0RXD)
#define SCL_PIN 1 // GPIO 1 (U0TXD)

// Buttons - avoid strapping pins where possible
#define BTN_UP 13     // GPIO 13
#define BTN_DOWN 14   // GPIO 14
#define BTN_SELECT 15 // GPIO 15

// Flash LED - MUST be GPIO 4 on ESP32-CAM
#define FLASH_PIN 4

// Flash / haptic PWM levels for analogWrite (0-255)
const uint8_t FLASH_DUTY_OFF = 0;
const uint8_t FLASH_DUTY_ON = 255;    // full brightness
const uint8_t FLASH_DUTY_HAPTIC = 40; // low level just for haptic

// Battery monitoing - Use GPIO 33 (not used by camera as data pin here)
#define BAT_PIN 16

/* ================= DISPLAY ================= */
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire);

/* ================= GLOBALS ================= */
Preferences prefs;
WebServer server(80);

bool wifiConnected = false;
bool screenOn = true;
bool flashlightOn = false;
int menuIndex = 0;
unsigned long lastInteraction = 0;
bool sdCardPresent = false;
bool adjustingBrightness = false;
unsigned long lastHomeRefresh = 0;

// Stopwatch state
bool stopwatchRunning = false;
unsigned long stopwatchBaseMillis = 0;  // millis() at last start
unsigned long stopwatchAccumulated = 0; // ms accumulated while paused
unsigned long lastStopwatchRefresh = 0;

// Simple screen state tracking
#define SCREEN_HOME 0
#define SCREEN_MENU 1
#define SCREEN_OTHER 2
#define SCREEN_STOPWATCH 3
int currentScreen = SCREEN_HOME;

// Photo counter
int photoCounter = 0;
const char *photoFolder = "/photos";

// Timezone: GMT+0 (Set your Timezone offset)
const long GMT_OFFSET_SEC = 0 * 3600; // Change 0 to your GMT+ offset
const int DAYLIGHT_OFFSET_SEC = 0;

// Weather cache
String weatherSummary = "";
String weatherDate = "";

/* ================= MENU ================= */
const char *menuItems[] = {
    "Take Picture",
    "Flashlight OFF",
    "WiFi Status",
    "SD Card Info",
    "Weather",
    "Brightness",
    "Stopwatch",
    "Sleep"};
#define MENU_COUNT 8

/* ================= CAMERA CONFIG ================= */
camera_config_t camera_cfg = {
    .pin_pwdn = 32,
    .pin_reset = -1,
    .pin_xclk = 0,
    .pin_sscb_sda = 26,
    .pin_sscb_scl = 27,

    .pin_d7 = 35,
    .pin_d6 = 34,
    .pin_d5 = 39,
    .pin_d4 = 36,
    .pin_d3 = 21,
    .pin_d2 = 19,
    .pin_d1 = 18,
    .pin_d0 = 5,
    .pin_vsync = 25,
    .pin_href = 23,
    .pin_pclk = 22,

    .xclk_freq_hz = 20000000,
    .ledc_timer = LEDC_TIMER_0,
    .ledc_channel = LEDC_CHANNEL_0,
    .pixel_format = PIXFORMAT_JPEG,
    .frame_size = FRAMESIZE_VGA, // 640x480 (good balance)
    .jpeg_quality = 12,
    .fb_count = 1 // Reduce to 1 for stability
};

/* ================= UTIL ================= */
void haptic()
{
    // Brief low-brightness pulse on flash pin for haptic
    uint8_t restore = flashlightOn ? FLASH_DUTY_ON : FLASH_DUTY_OFF;
    analogWrite(FLASH_PIN, FLASH_DUTY_HAPTIC);
    delay(20);
    analogWrite(FLASH_PIN, restore);
}

int batteryPercent()
{
    // Simple voltage reading
    int raw = analogRead(BAT_PIN);
    // Assuming 2:1 voltage divider (100k+100k)
    float voltage = raw * (3.3 / 4095.0) * 2.0;
    int pct = (voltage - 3.0) / (4.2 - 3.0) * 100;
    return constrain(pct, 0, 100);
}

// Brightness control for OLED (0-255)
const uint8_t BRIGHTNESS_LEVELS[] = {40, 80, 140, 200};
const int BRIGHTNESS_LEVEL_COUNT = 4;
int brightnessLevelIndex = 1;      // start near the middle
const uint8_t BRIGHTNESS_DIM = 10; // dim level for sleep

void set_display_brightness(uint8_t value)
{
    display.ssd1306_command(SSD1306_SETCONTRAST);
    display.ssd1306_command(value);
}

void applyCurrentBrightness()
{
    set_display_brightness(BRIGHTNESS_LEVELS[brightnessLevelIndex]);
}

String timeStr()
{
    struct tm t;
    if (!getLocalTime(&t))
        return "--:--";
    char buf[6];
    strftime(buf, sizeof(buf), "%H:%M", &t);
    return String(buf);
}

String dateTimeStr()
{
    struct tm t;
    if (!getLocalTime(&t))
        return "--";
    char buf[20];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &t);
    return String(buf);
}

String longDateStr()
{
    struct tm t;
    if (!getLocalTime(&t))
        return "--";

    static const char *WEEKDAYS[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    static const char *MONTHS[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

    int day = t.tm_mday;
    const char *suffix = "th";
    if (day % 10 == 1 && day % 100 != 11)
        suffix = "st";
    else if (day % 10 == 2 && day % 100 != 12)
        suffix = "nd";
    else if (day % 10 == 3 && day % 100 != 13)
        suffix = "rd";

    char buf[40];
    snprintf(buf, sizeof(buf), "%s %d%s %s %02d",
             WEEKDAYS[t.tm_wday], day, suffix, MONTHS[t.tm_mon], (t.tm_year + 1900) % 100);
    return String(buf);
}

unsigned long stopwatchElapsedMs()
{
    if (stopwatchRunning)
    {
        return stopwatchAccumulated + (millis() - stopwatchBaseMillis);
    }
    else
    {
        return stopwatchAccumulated;
    }
}

String stopwatchTimeStr()
{
    unsigned long ms = stopwatchElapsedMs();
    unsigned long totalSeconds = ms / 1000;
    unsigned long minutes = totalSeconds / 60;
    unsigned long seconds = totalSeconds % 60;
    unsigned long tenths = (ms % 1000) / 100;

    char buf[16];
    snprintf(buf, sizeof(buf), "%02lu:%02lu.%01lu", minutes, seconds, tenths);
    return String(buf);
}

String todayDateKey()
{
    struct tm t;
    if (!getLocalTime(&t))
        return "";
    char buf[11];
    strftime(buf, sizeof(buf), "%Y-%m-%d", &t);
    return String(buf);
}

bool loadWeatherFromSD()
{
    if (!sdCardPresent)
        return false;

    File file = SD_MMC.open("/weather.json", FILE_READ);
    if (!file)
        return false;

    StaticJsonDocument<256> doc;
    DeserializationError err = deserializeJson(doc, file);
    file.close();
    if (err)
        return false;

    String date = doc["date"].as<String>();
    if (date != todayDateKey())
        return false;

    weatherSummary = doc["summary"].as<String>();
    weatherDate = date;
    return weatherSummary.length() > 0;
}

bool fetchAndCacheWeather()
{
    if (!wifiConnected || !sdCardPresent)
        return false;

    // Require the user to set a valid OpenWeatherMap API key
    const char *WEATHER_API_KEY = "YOUR_API_KEY_HERE"; // Replace
    if (String(WEATHER_API_KEY) == "YOUR_API_KEY_HERE")
        return false;

    String url = String("http://api.openweathermap.org/data/2.5/weather?q=YOUR_LOCATION,COUNTRY_CODE&units=metric&appid=") + WEATHER_API_KEY; // Set your Location

    HTTPClient http;
    if (!http.begin(url))
        return false;

    int code = http.GET();
    if (code != 200)
    {
        http.end();
        return false;
    }

    String payload = http.getString();
    http.end();

    DynamicJsonDocument doc(2048);
    DeserializationError err = deserializeJson(doc, payload);
    if (err)
        return false;

    float temp = doc["main"]["temp"] | 0.0;
    const char *desc = doc["weather"][0]["main"] | "";

    int tempRounded = (int)roundf(temp);
    weatherSummary = String(tempRounded) + "C " + String(desc);
    weatherDate = todayDateKey();

    // Save compact JSON to SD
    StaticJsonDocument<256> out;
    out["date"] = weatherDate;
    out["summary"] = weatherSummary;

    File file = SD_MMC.open("/weather.json", FILE_WRITE);
    if (!file)
        return false;
    serializeJson(out, file);
    file.close();

    return true;
}

String getWiFiStatus()
{
    if (WiFi.status() == WL_CONNECTED)
    {
        return WiFi.SSID() + "\nIP: " + WiFi.localIP().toString();
    }
    return "WiFi Disconnected";
}

/* ================= UI ================= */
void showHome()
{
    currentScreen = SCREEN_HOME;
    display.clearDisplay();
    // Greeting and date
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.println("Hi Umar,");
    display.println(longDateStr());

    // Time in larger font
    display.setTextSize(2);
    display.setCursor(0, 24);
    display.println(timeStr());

    // Battery and photos summary (small, bottom row)
    display.setTextSize(1);
    display.setCursor(0, 48);
    display.print("Bat:");
    display.print(batteryPercent());
    display.print("%  P:");
    display.print(photoCounter);
    display.display();
}

void showMenu()
{
    currentScreen = SCREEN_MENU;
    // Update flashlight menu text
    const char *flashlightText = flashlightOn ? "Flashlight ON" : "Flashlight OFF";

    display.clearDisplay();
    for (int i = 0; i < MENU_COUNT; i++)
    {
        display.setCursor(0, i * 10);
        display.print(i == menuIndex ? "> " : "  ");

        // Show correct flashlight text and dynamic entries
        if (i == 1)
        {
            display.println(flashlightText);
        }
        else if (i == 5)
        {
            display.print("Brightness ");
            display.println(brightnessLevelIndex + 1);
        }
        else
        {
            display.println(menuItems[i]);
        }
    }
    display.display();
}

void showBrightnessAdjust()
{
    currentScreen = SCREEN_OTHER;
    display.clearDisplay();
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.println("Brightness");
    display.println();

    display.setTextSize(2);
    display.setCursor(0, 20);
    display.print("Level ");
    display.println(brightnessLevelIndex + 1);

    display.setTextSize(1);
    display.setCursor(0, 48);
    display.println("UP/DOWN adjust, SEL to exit");
    display.display();
}

void showSDInfo()
{
    currentScreen = SCREEN_OTHER;
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("SD Card Info:");
    display.println();

    if (sdCardPresent)
    {
        uint64_t cardSize = SD_MMC.cardSize() / (1024 * 1024);
        uint64_t usedSpace = SD_MMC.usedBytes() / (1024 * 1024);
        uint64_t freeSpace = cardSize - usedSpace;

        display.print("Size: ");
        display.print(cardSize);
        display.println(" MB");

        display.print("Free: ");
        display.print(freeSpace);
        display.println(" MB");

        display.print("Photos: ");
        display.println(photoCounter);
    }
    else
    {
        display.println("No SD Card!");
    }

    display.display();
    delay(3000);
    showMenu();
}

void showWeatherScreen()
{
    currentScreen = SCREEN_OTHER;
    display.clearDisplay();
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.println("Weather - ISB");
    display.println();

    if (weatherSummary.length())
    {
        display.setTextSize(2);
        display.setCursor(0, 20);
        display.println(weatherSummary);

        display.setTextSize(1);
        display.setCursor(0, 48);
        display.print("Date ");
        display.println(weatherDate);
    }
    else
    {
        display.println("No weather data.");
        display.println("Check WiFi/API key.");
    }

    display.display();
    delay(3000);
    showMenu();
}

void showStopwatchScreen()
{
    currentScreen = SCREEN_STOPWATCH;
    display.clearDisplay();

    display.setTextSize(1);
    display.setCursor(0, 0);
    display.println("Stopwatch");

    display.setTextSize(2);
    display.setCursor(0, 16);
    display.println(stopwatchTimeStr());

    display.setTextSize(1);
    display.setCursor(0, 48);
    if (stopwatchRunning)
    {
        display.println("SEL: Pause  DN: Reset");
    }
    else
    {
        display.println("SEL: Start DN: Reset");
    }
    display.setCursor(0, 56);
    display.println("UP: Back to Menu");

    display.display();
}

/* ================= SD CARD FUNCTIONS ================= */
bool initSDCard()
{
    Serial.println("Initializing SD card...");

    if (!SD_MMC.begin("/sdcard", true))
    { // 1-bit mode
        Serial.println("SD Card Mount Failed");
        return false;
    }

    uint8_t cardType = SD_MMC.cardType();
    if (cardType == CARD_NONE)
    {
        Serial.println("No SD card");
        return false;
    }

    // Create photos directory
    if (!SD_MMC.exists(photoFolder))
    {
        SD_MMC.mkdir(photoFolder);
    }

    // Count existing photos
    File root = SD_MMC.open(photoFolder);
    if (root)
    {
        int maxCount = 0;
        while (File file = root.openNextFile())
        {
            String filename = file.name();
            if (filename.endsWith(".jpg"))
            {
                // Extract number from filename like "1.jpg"
                int num = filename.substring(0, filename.indexOf('.')).toInt();
                if (num > maxCount)
                    maxCount = num;
            }
            file.close();
        }
        photoCounter = maxCount;
        root.close();
    }

    Serial.print("SD OK - Photos: ");
    Serial.println(photoCounter);
    return true;
}

String savePhotoToSD(camera_fb_t *fb)
{
    if (!sdCardPresent)
        return "";

    photoCounter++;
    char filename[32];
    sprintf(filename, "%s/%d.jpg", photoFolder, photoCounter);

    File file = SD_MMC.open(filename, FILE_WRITE);
    if (!file)
    {
        Serial.println("Failed to open file");
        return "";
    }

    bool success = file.write(fb->buf, fb->len) == fb->len;
    file.close();

    if (!success)
    {
        SD_MMC.remove(filename);
        photoCounter--; // Roll back counter
        return "";
    }

    Serial.print("Saved: ");
    Serial.println(filename);
    return String(filename);
}

/* ================= CAMERA ================= */
void takePicture()
{
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("Capturing...");
    display.display();

    camera_fb_t *fb = esp_camera_fb_get();

    if (fb)
    {
        display.println("Saving to SD...");
        display.display();

        String filename = savePhotoToSD(fb);
        if (filename.length() > 0)
        {
            display.clearDisplay();
            display.println("Photo Saved!");
            display.print("File: ");
            display.println(filename.substring(filename.lastIndexOf('/') + 1));
            display.print("Size: ");
            display.print(fb->len / 1024);
            display.println(" KB");
        }
        else
        {
            display.println("Save failed!");
        }

        esp_camera_fb_return(fb);
    }
    else
    {
        display.println("Camera error!");
    }

    display.display();
    delay(2000);
    showMenu();
}

/* ================= WEB SERVER ================= */
void handleRoot()
{
    String html = "<!DOCTYPE html><html><head><meta charset='utf-8'>";
    html += "<title>ESP32-CAM Watch</title>";
    html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
    html += "<style>";
    html += "body{font-family:-apple-system,Arial,sans-serif;margin:0;background:#0f172a;color:#e5e7eb;}";
    html += ".header{padding:16px 24px;background:#111827;display:flex;justify-content:space-between;align-items:center;}";
    html += ".title{font-size:20px;font-weight:600;}";
    html += ".subtitle{font-size:13px;color:#9ca3af;}";
    html += ".container{padding:20px;max-width:900px;margin:0 auto;}";
    html += ".cards{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:16px;}";
    html += ".card{background:#111827;border-radius:14px;padding:16px;box-shadow:0 10px 25px rgba(0,0,0,0.45);}";
    html += ".card h2{margin:0 0 8px;font-size:16px;}";
    html += ".metric{font-size:22px;font-weight:600;margin:4px 0;}";
    html += ".label{font-size:12px;color:#9ca3af;}";
    html += ".button{display:inline-block;margin-top:8px;padding:8px 14px;border-radius:999px;background:#2563eb;color:white;text-decoration:none;font-size:13px;}";
    html += ".button.secondary{background:#374151;}";
    html += ".photo-grid{display:flex;flex-wrap:wrap;gap:8px;margin-top:8px;max-height:260px;overflow-y:auto;}";
    html += ".photo-link{display:inline-block;padding:6px 10px;border-radius:999px;background:#1f2937;color:#e5e7eb;font-size:12px;text-decoration:none;}";
    html += ".photo-link:hover{background:#2563eb;}";
    html += ".preview{margin-top:8px;border-radius:10px;max-width:100%;height:auto;border:1px solid #1f2937;}";
    html += "input{width:100%;padding:6px 8px;border-radius:8px;border:1px solid #374151;background:#020617;color:#e5e7eb;margin-top:4px;font-size:13px;}";
    html += "label{font-size:12px;color:#9ca3af;}";
    html += "form{display:grid;gap:10px;margin-top:8px;}";
    html += ".badge{display:inline-block;padding:4px 10px;border-radius:999px;background:#1f2937;font-size:11px;}";
    html += "</style></head><body>";

    html += "<div class='header'><div><div class='title'>Hi Umar  how can I help?</div>";
    html += "<div class='subtitle'>ESP32-CAM Watch dashboard</div></div>";
    html += "<div class='subtitle'>" + dateTimeStr() + "</div></div>";

    html += "<div class='container'><div class='cards'>";

    // Status card
    html += "<div class='card'><h2>Status</h2>";
    if (wifiConnected)
    {
        html += "<div class='metric'>Connected</div>";
        html += "<div class='label'>SSID: " + WiFi.SSID() + "<br>IP: " + WiFi.localIP().toString() + "</div>";
    }
    else
    {
        html += "<div class='metric'>AP Mode</div>";
        html += "<div class='label'>SSID: ESP32-Watch<br>PW: 12345678</div>";
    }
    html += "<a class='button' href='/take'>Take Photo Now</a>";
    html += "</div>";

    // Battery / photos card
    html += "<div class='card'><h2>Device</h2>";
    html += "<div class='metric'>" + String(batteryPercent()) + "%</div>";
    html += "<div class='label'>Battery · Photos: " + String(photoCounter) + "</div>";
    if (photoCounter > 0 && sdCardPresent)
    {
        html += "<img class='preview' src='/latest.jpg' alt='Latest photo'>";
    }
    html += "</div>";

    // Weather card
    html += "<div class='card'><h2>Islamabad Weather</h2>";
    if (weatherSummary.length())
    {
        html += "<div class='metric'>" + weatherSummary + "</div>";
        html += "<div class='label'>Cached for <span class='badge'>" + weatherDate + "</span></div>";
    }
    else
    {
        html += "<div class='label'>Weather not available. Check API key and SD card.</div>";
    }
    html += "</div>";

    // WiFi setup card
    html += "<div class='card'><h2>WiFi Setup</h2>";
    html += "<form action='/save'>";
    html += "<div><label>SSID</label><input name='s' placeholder='Network name'></div>";
    html += "<div><label>Password</label><input name='p' type='password' placeholder='Password'></div>";
    html += "<button class='button secondary' type='submit'>Save &amp; Reboot</button>";
    html += "</form></div>";

    // Photos gallery card
    html += "<div class='card'><h2>Photo Gallery</h2>";
    if (!sdCardPresent)
    {
        html += "<div class='label'>No SD card detected.</div>";
    }
    else if (photoCounter == 0)
    {
        html += "<div class='label'>No photos yet. Capture one to get started.</div>";
    }
    else
    {
        html += "<div class='label'>Tap a filename to view the full image.</div>";
        html += "<div class='photo-grid'>";

        File root = SD_MMC.open(photoFolder);
        if (root)
        {
            while (true)
            {
                File file = root.openNextFile();
                if (!file)
                    break;
                String name = file.name();
                if (name.endsWith(".jpg"))
                {
                    String base = name.substring(name.lastIndexOf('/') + 1);
                    html += "<a class='photo-link' href='/photo?n=" + base + "'>" + base + "</a>";
                }
                file.close();
            }
            root.close();
        }

        html += "</div>";
    }
    html += "</div>"; // end gallery card

    html += "</div></div></body></html>";
    server.send(200, "text/html", html);
}

void handlePhotoFile()
{
    if (!sdCardPresent)
    {
        server.send(404, "text/plain", "No SD card");
        return;
    }

    String name = server.arg("n");
    if (!name.endsWith(".jpg"))
    {
        name += ".jpg";
    }

    String path = String(photoFolder) + "/" + name;
    File file = SD_MMC.open(path, FILE_READ);
    if (!file || file.isDirectory())
    {
        server.send(404, "text/plain", "Photo not found");
        if (file)
            file.close();
        return;
    }

    server.streamFile(file, "image/jpeg");
    file.close();
}

void handleLatestPhoto()
{
    if (!sdCardPresent || photoCounter == 0)
    {
        server.send(404, "text/plain", "No photos yet");
        return;
    }

    char filename[32];
    sprintf(filename, "%s/%d.jpg", photoFolder, photoCounter);

    File file = SD_MMC.open(filename, FILE_READ);
    if (!file || file.isDirectory())
    {
        server.send(404, "text/plain", "Photo not found");
        if (file)
            file.close();
        return;
    }

    server.streamFile(file, "image/jpeg");
    file.close();
}

void handleSave()
{
    prefs.begin("wifi", false);
    prefs.putString("ssid", server.arg("s"));
    prefs.putString("pass", server.arg("p"));
    prefs.end();

    server.send(200, "text/html",
                "<html><body>"
                "<h2>Settings Saved!</h2>"
                "<p>Rebooting...</p>"
                "</body></html>");

    delay(1000);
    ESP.restart();
}

void handleTakePhoto()
{
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb)
    {
        String filename = savePhotoToSD(fb);
        esp_camera_fb_return(fb);

        server.send(200, "text/html",
                    "<html><body>"
                    "<h2>Photo Taken!</h2>"
                    "<p>Saved as: " +
                        filename + "</p>"
                                   "<a href='/'>Back</a>"
                                   "</body></html>");
    }
    else
    {
        server.send(500, "text/plain", "Camera error");
    }
}

void startAPMode()
{
    WiFi.softAP("ESP32-Watch", "12345678");
    server.on("/", handleRoot);
    server.on("/save", handleSave);
    server.on("/take", handleTakePhoto);
    server.on("/photo", handlePhotoFile);
    server.on("/latest.jpg", handleLatestPhoto);
    server.begin();

    display.clearDisplay();
    display.println("AP Mode Active");
    display.print("SSID: ESP32-Watch");
    display.println("PW: 12345678");
    display.print("IP: ");
    display.println(WiFi.softAPIP());
    display.display();
    delay(2000);
}

void connectWiFi()
{
    prefs.begin("wifi", true);
    if (!prefs.isKey("ssid"))
    {
        prefs.end();
        startAPMode();
        wifiConnected = false;
        return;
    }

    String ssid = prefs.getString("ssid");
    String pass = prefs.getString("pass");
    prefs.end();

    WiFi.begin(ssid.c_str(), pass.c_str());

    display.clearDisplay();
    display.println("Connecting to:");
    display.println(ssid);
    display.display();

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 30)
    {
        delay(500);
        display.print(".");
        display.display();
        attempts++;
    }

    if (WiFi.status() == WL_CONNECTED)
    {
        wifiConnected = true;
        
        configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, "pool.ntp.org");

        // Wait briefly for NTP time sync so RTC has valid time
        struct tm timeinfo;
        int retries = 0;
        while (!getLocalTime(&timeinfo) && retries < 20)
        {
            delay(500);
            retries++;
        }

        // Load today's weather from SD or fetch and cache once
        if (!loadWeatherFromSD())
        {
            fetchAndCacheWeather();
        }

        server.on("/", handleRoot);
        server.on("/save", handleSave);
        server.on("/take", handleTakePhoto);
        server.on("/photo", handlePhotoFile);
        server.on("/latest.jpg", handleLatestPhoto);
        server.begin();

        display.clearDisplay();
        display.println("WiFi Connected!");
        display.print("IP: ");
        display.println(WiFi.localIP());
        display.display();
        delay(2000);
    }
    else
    {
        wifiConnected = false;
        startAPMode();
    }
}

/* ================= SETUP ================= */
void setup()
{
    Serial.begin(115200);
    delay(1000);
    Serial.println("\nESP32-CAM Watch Starting...");

    // Setup pins
    pinMode(BTN_UP, INPUT_PULLUP);
    pinMode(BTN_DOWN, INPUT_PULLUP);
    pinMode(BTN_SELECT, INPUT_PULLUP);
    pinMode(FLASH_PIN, OUTPUT);
    digitalWrite(FLASH_PIN, LOW);
    analogWrite(FLASH_PIN, FLASH_DUTY_OFF); // ensure PWM is off

    // Initialize OLED
    Wire.begin(SDA_PIN, SCL_PIN);
    if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C))
    {
        Serial.println("OLED failed!");
        while (1)
            ;
    }
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);

    display.println("Initializing...");
    display.display();
    // Set default brightness
    applyCurrentBrightness();

    // Initialize camera
    esp_err_t err = esp_camera_init(&camera_cfg);
    if (err != ESP_OK)
    {
        display.clearDisplay();
        display.println("Camera init fail");
        display.display();
        delay(2000);
        // Continue without camera
    }
    else
    {
        Serial.println("Camera OK");
    }

    // Initialize SD card
    sdCardPresent = initSDCard();

    // Connect WiFi
    connectWiFi();

    showHome();
    lastHomeRefresh = millis();
    lastInteraction = millis();
}

/* ================= LOOP ================= */
void loop()
{
    server.handleClient();

    // Check if screen should wake up (button press)
    if (!screenOn && (!digitalRead(BTN_UP) || !digitalRead(BTN_DOWN) || !digitalRead(BTN_SELECT)))
    {
        screenOn = true;
        adjustingBrightness = false;
        applyCurrentBrightness(); // restore full brightness
        showHome();
        lastHomeRefresh = millis();
        lastInteraction = millis();
        delay(200); // Debounce
    }

    if (screenOn)
    {
        unsigned long now = millis();

        // Periodically refresh home screen time/date while on home
        if (currentScreen == SCREEN_HOME && now - lastHomeRefresh > 1000)
        {
            showHome();
            lastHomeRefresh = now;
        }

        // Periodically refresh stopwatch display while in stopwatch screen
        if (currentScreen == SCREEN_STOPWATCH && now - lastStopwatchRefresh > 100)
        {
            showStopwatchScreen();
            lastStopwatchRefresh = now;
        }

        // Button UP
        if (!digitalRead(BTN_UP))
        {
            haptic();
            if (adjustingBrightness)
            {
                brightnessLevelIndex = (brightnessLevelIndex - 1 + BRIGHTNESS_LEVEL_COUNT) % BRIGHTNESS_LEVEL_COUNT;
                applyCurrentBrightness();
                showBrightnessAdjust();
            }
            else if (currentScreen == SCREEN_STOPWATCH)
            {
                // UP exits stopwatch back to menu
                showMenu();
            }
            else
            {
                menuIndex = (menuIndex - 1 + MENU_COUNT) % MENU_COUNT;
                showMenu();
            }
            delay(250);
            lastInteraction = millis();
        }

        // Button DOWN
        if (!digitalRead(BTN_DOWN))
        {
            haptic();
            if (adjustingBrightness)
            {
                brightnessLevelIndex = (brightnessLevelIndex + 1) % BRIGHTNESS_LEVEL_COUNT;
                applyCurrentBrightness();
                showBrightnessAdjust();
            }
            else if (currentScreen == SCREEN_STOPWATCH)
            {
                // Reset stopwatch when stopped
                if (!stopwatchRunning)
                {
                    stopwatchAccumulated = 0;
                    showStopwatchScreen();
                }
            }
            else
            {
                menuIndex = (menuIndex + 1) % MENU_COUNT;
                showMenu();
            }
            delay(250);
            lastInteraction = millis();
        }

        // Button SELECT
        if (!digitalRead(BTN_SELECT))
        {
            haptic();
            if (adjustingBrightness)
            {
                // Exit brightness adjust mode back to menu
                adjustingBrightness = false;
                showMenu();
            }
            else if (currentScreen == SCREEN_STOPWATCH)
            {
                // Toggle start/pause
                if (!stopwatchRunning)
                {
                    stopwatchBaseMillis = millis();
                    stopwatchRunning = true;
                }
                else
                {
                    stopwatchAccumulated = stopwatchElapsedMs();
                    stopwatchRunning = false;
                }
                showStopwatchScreen();
            }
            else
            {
                switch (menuIndex)
                {
                case 0: // Take Picture
                    takePicture();
                    break;

                case 1: // Toggle Flashlight
                    flashlightOn = !flashlightOn;
                    analogWrite(FLASH_PIN, flashlightOn ? FLASH_DUTY_ON : FLASH_DUTY_OFF);
                    showMenu();
                    break;

                case 2: // WiFi Status
                    display.clearDisplay();
                    display.println("WiFi Status:");
                    display.println(getWiFiStatus());
                    display.display();
                    delay(3000);
                    showMenu();
                    break;

                case 3: // SD Card Info
                    showSDInfo();
                    break;

                case 4: // Weather
                    showWeatherScreen();
                    break;

                case 5: // Brightness
                    adjustingBrightness = true;
                    showBrightnessAdjust();
                    break;

                case 6: // Stopwatch
                    showStopwatchScreen();
                    lastStopwatchRefresh = millis();
                    break;

                case 7: // Sleep
                    screenOn = false;
                    analogWrite(FLASH_PIN, FLASH_DUTY_OFF);
                    flashlightOn = false;
                    // Dim the display instead of fully clearing/off
                    set_display_brightness(BRIGHTNESS_DIM);
                    break;
                }
            }

            lastInteraction = millis();
        }

        // Auto sleep after 30 seconds
        if (millis() - lastInteraction > 30000)
        {
            screenOn = false;
            analogWrite(FLASH_PIN, FLASH_DUTY_OFF);
            flashlightOn = false;
            adjustingBrightness = false;
            // Dim the display on inactivity
            set_display_brightness(BRIGHTNESS_DIM);
        }
    }

    delay(50); // Small delay to prevent CPU hogging
}
