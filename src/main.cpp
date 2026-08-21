/**
 * Moon Phase
 * ESP32 WROOM + GC9A01 240x240 round (SPI, TFT_eSPI) + LVGL 8.3.11
 * Credit to nishad2m8 https://github.com/nishad2m8
 * What this does:
 *  - Connects to WiFi, syncs time via NTP
 *  - Updates the time/date labels every second from the system clock
 *  - Periodically fetches the current moon age from NASA's Dial-a-Moon API
 *    and updates the phase name label + moon image accordingly
 *
 * NOTE: TFT pin mapping lives in platformio.ini build_flags.
 * Fill in your WiFi details in include/credentials.h before uploading.
 */

#include <Arduino.h>
#include <lvgl.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include "ui.h"
#include "credentials.h"

TFT_eSPI tft = TFT_eSPI();

// ---- Time zone ----

// POSIX TZ format: "<+06>-6" means UTC+6, no DST. use "<+01>-1" for UTC +1, "<-05>5" for UTC -5
const char *TIMEZONE = "<+06>-6"; 
const char *NTP_SERVER = "pool.ntp.org";

// ---- LVGL display buffer (partial, single buffer, internal RAM only) ----
static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf1[240 * 40];
static lv_disp_drv_t disp_drv;

static void disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p)
{
    uint32_t w = (area->x2 - area->x1 + 1);
    uint32_t h = (area->y2 - area->y1 + 1);

    tft.startWrite();
    tft.setAddrWindow(area->x1, area->y1, w, h);
    tft.pushColors((uint16_t *)&color_p->full, w * h, true);
    tft.endWrite();

    lv_disp_flush_ready(disp);
}


static const lv_img_dsc_t *moon_frames[30] = {
    &ui_img_moon_moon_1_png,  &ui_img_moon_moon_2_png,  &ui_img_moon_moon_3_png,
    &ui_img_moon_moon_4_png,  &ui_img_moon_moon_5_png,  &ui_img_moon_moon_6_png,
    &ui_img_moon_moon_7_png,  &ui_img_moon_moon_8_png,  &ui_img_moon_moon_9_png,
    &ui_img_moon_moon_10_png, &ui_img_moon_moon_11_png, &ui_img_moon_moon_12_png,
    &ui_img_moon_moon_13_png, &ui_img_moon_moon_14_png, &ui_img_moon_moon_15_png,
    &ui_img_moon_moon_16_png, &ui_img_moon_moon_17_png, &ui_img_moon_moon_18_png,
    &ui_img_moon_moon_19_png, &ui_img_moon_moon_20_png, &ui_img_moon_moon_21_png,
    &ui_img_moon_moon_22_png, &ui_img_moon_moon_23_png, &ui_img_moon_moon_24_png,
    &ui_img_moon_moon_25_png, &ui_img_moon_moon_26_png, &ui_img_moon_moon_27_png,
    &ui_img_moon_moon_28_png, &ui_img_moon_moon_29_png, &ui_img_moon_moon_30_png,
};

// ---- Update timers ----
const uint32_t CLOCK_UPDATE_MS = 1000;                 // refresh time/date label every second
const uint32_t MOON_UPDATE_MS = 60UL * 60UL * 1000UL;  // refetch moon phase hourly once we have a
                                                        // successful reading (moon age barely moves
                                                        // minute to minute)
const uint32_t MOON_RETRY_MS = 15UL * 1000UL;          // retry this often until the FIRST fetch succeeds

static uint32_t lastClockMillis = 0;
static uint32_t lastMoonMillis = 0;
static bool moonDataValid = false;

// ---- Forward declarations ----
void connectToWiFi();
bool waitForTimeSync(uint32_t timeoutMs);
void updateClockLabels();
void updateMoonData();
String getMoonPhase(double age);
int getMoonImageIndex(double age);
void setMoonImage(int index);
void playMoonIntroAnimation();

void setup()
{
    Serial.begin(115200);
    tft.begin();
    tft.setRotation(2); // Try 0 if image is upside down.
    tft.fillScreen(TFT_BLACK);
    lv_init();
    lv_disp_draw_buf_init(&draw_buf, buf1, NULL, 240 * 40);

    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = 240;
    disp_drv.ver_res = 240;
    disp_drv.flush_cb = disp_flush;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    ui_init();
    lv_task_handler(); 
    playMoonIntroAnimation();
    connectToWiFi();
    configTzTime(TIMEZONE, NTP_SERVER);
    waitForTimeSync(15000); 
    updateMoonData();
    lastClockMillis = millis();
    lastMoonMillis = millis();
    Serial.println("Setup complete.");
}

void loop()
{
    lv_task_handler();
    uint32_t now = millis();
    if (now - lastClockMillis >= CLOCK_UPDATE_MS) {
        lastClockMillis = now;
        updateClockLabels();
    }

    if (now - lastMoonMillis >= (moonDataValid ? MOON_UPDATE_MS : MOON_RETRY_MS)) {
        lastMoonMillis = now;
        updateMoonData();
    }

    delay(5);
}

void connectToWiFi()
{
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.print("Connecting to WiFi");
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
        delay(500);
        Serial.print(".");
    }
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println(" connected.");
    } else {
        Serial.println(" failed to connect within 20s -- continuing without WiFi for now.");
    }
}

bool waitForTimeSync(uint32_t timeoutMs)
{
    Serial.print("Waiting for NTP time sync");
    uint32_t start = millis();
    time_t now;
    struct tm timeinfo;

    while (millis() - start < timeoutMs) {
        time(&now);
        localtime_r(&now, &timeinfo);
        if (timeinfo.tm_year >= (2023 - 1900)) {
            Serial.println(" synced.");
            return true;
        }
        delay(500);
        Serial.print(".");
    }
    Serial.println(" timed out -- will keep retrying in the background.");
    return false;
}

void updateClockLabels()
{
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    if (timeinfo.tm_year < (2023 - 1900)) {
        // Time not synced yet
        return;
    }

    char timeStr[9]; // "HH:MM:SS"
    snprintf(timeStr, sizeof(timeStr), "%02d:%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);

    char dateStr[11]; // "13 Aug 26"
    strftime(dateStr, sizeof(dateStr), "%d %b %y", &timeinfo);

    lv_label_set_text(ui_time, timeStr);
    lv_label_set_text(ui_date, dateStr);
    lv_obj_invalidate(ui_time);
    lv_obj_invalidate(ui_date);
    lv_refr_now(NULL);
}

void setMoonImage(int index)
{
    if (index < 0) index = 0;
    if (index > 29) index = 29;
    lv_img_set_src(ui_img_moon, moon_frames[index]);
    lv_obj_invalidate(ui_img_moon);
    lv_refr_now(NULL); 
}

void playMoonIntroAnimation()
{
    const uint16_t frameDelayMs = 100; 
    for (int i = 0; i < 30; i++) {
        setMoonImage(i);
        delay(frameDelayMs);
    }
}

void updateMoonData()
{
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi not connected. Skipping moon data fetch.");
        return;
    }

    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    if (timeinfo.tm_year < (2023 - 1900)) {
        Serial.println("Time not set yet. Skipping moon data fetch.");
        return;
    }

    char dateStr[25];
    if (strftime(dateStr, sizeof(dateStr), "%Y-%m-%dT%H:%M", &timeinfo) == 0) {
        Serial.println("Failed to format date string.");
        return;
    }

    String url = "https://svs.gsfc.nasa.gov/api/dialamoon/";
    url += dateStr;

    Serial.print("Fetching moon data: ");
    Serial.println(url);

    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    http.begin(client, url);
    int httpCode = http.GET();

    if (httpCode == HTTP_CODE_OK) {
        String payload = http.getString();

        DynamicJsonDocument doc(2048);
        DeserializationError err = deserializeJson(doc, payload);
        if (err) {
            Serial.print("JSON parse failed: ");
            Serial.println(err.c_str());
            http.end();
            return;
        }

        double age = doc["age"].as<double>();
        Serial.printf("Moon age: %.3f days\n", age);

        String phaseName = getMoonPhase(age);
        lv_label_set_text(ui_phase, phaseName.c_str());
        lv_obj_invalidate(ui_phase);
        lv_refr_now(NULL);

        setMoonImage(getMoonImageIndex(age));
        moonDataValid = true;

    } else {
        Serial.printf("Failed to fetch moon data, HTTP code: %d\n", httpCode);
    }

    http.end();
}

String getMoonPhase(double age)
{
    if (age < 1.84566) return "New Moon";
    else if (age < 5.53699) return "Waxing Crescent";
    else if (age < 9.22831) return "First Quarter";
    else if (age < 12.91963) return "Waxing Gibbous";
    else if (age < 16.61096) return "Full Moon";
    else if (age < 20.30228) return "Waning Gibbous";
    else if (age < 23.99361) return "Last Quarter";
    else return "Waning Crescent";
}

int getMoonImageIndex(double age)
{

    int idx = (int)(age / 29.53 * 30);
    if (idx > 29) idx = 29;
    if (idx < 0) idx = 0;
    return idx;
}
