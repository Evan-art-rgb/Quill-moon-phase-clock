/**
 * Quill Moon Clock v1.1 - non-blocking edition
 * ESP32 WROOM + GC9A01 240x240 + LVGL 8.3.11
 * Based on Moon Phase Clock by nishad2m8: https://github.com/nishad2m8
 *
 * Quill changes:
 *  - Non-blocking Wi-Fi connection/reconnection state machine
 *  - Non-blocking NTP wait (system SNTP runs in background)
 *  - Non-blocking moon intro animation
 *  - NASA HTTPS/JSON work moved off the LVGL loop to a FreeRTOS worker task
 *  - LVGL is touched only by the main task
 */

#include <Arduino.h>
#include <lvgl.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "ui.h"
#include "secrets.h"

TFT_eSPI tft = TFT_eSPI();
const char *NTP_SERVER = "pool.ntp.org";

static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf1[240 * 40];
static lv_disp_drv_t disp_drv;

static void disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p)
{
    uint32_t w = area->x2 - area->x1 + 1;
    uint32_t h = area->y2 - area->y1 + 1;
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

constexpr uint32_t CLOCK_UPDATE_MS = 1000;
constexpr uint32_t MOON_UPDATE_MS = 60UL * 60UL * 1000UL;
constexpr uint32_t MOON_RETRY_MS = 15UL * 1000UL;
constexpr uint32_t WIFI_RETRY_MS = 30UL * 1000UL;
constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 20UL * 1000UL;
constexpr uint32_t INTRO_FRAME_MS = 100;

static uint32_t lastClockMillis = 0;
static uint32_t lastMoonAttemptMillis = 0;
static uint32_t wifiAttemptMillis = 0;
static bool moonDataValid = false;
static bool ntpConfigured = false;
static bool timeWasValid = false;

enum class WifiState { IDLE, CONNECTING, CONNECTED };
static WifiState wifiState = WifiState::IDLE;

static bool introActive = true;
static int introFrame = 0;
static uint32_t introFrameMillis = 0;

struct MoonResult {
    bool success;
    double age;
    int httpCode;
};

static QueueHandle_t moonResultQueue = nullptr;
static TaskHandle_t moonTaskHandle = nullptr;

bool timeIsValid();
void serviceWiFi();
void serviceClock();
void serviceIntro();
void serviceMoonFetch();
void moonFetchTask(void *parameter);
void updateClockLabels();
String getMoonPhase(double age);
int getMoonImageIndex(double age);
void setMoonImage(int index);

void setup()
{
    Serial.begin(115200);
    Serial.println("\nQuill Moon Clock v1.1 - non-blocking");

    tft.begin();
    tft.setRotation(0);
    tft.fillScreen(TFT_BLACK);

    lv_init();
    lv_disp_draw_buf_init(&draw_buf, buf1, nullptr, 240 * 40);
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = 240;
    disp_drv.ver_res = 240;
    disp_drv.flush_cb = disp_flush;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    ui_init();
    lv_task_handler();

    moonResultQueue = xQueueCreate(1, sizeof(MoonResult));
    if (!moonResultQueue) {
        Serial.println("ERROR: could not create moon result queue.");
    }

    // Start networking, but do not wait for it.
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    wifiAttemptMillis = millis();
    wifiState = WifiState::CONNECTING;
    Serial.println("Wi-Fi connection started in background.");

    introFrameMillis = millis();
    lastClockMillis = millis();
    // Allow first moon fetch as soon as time + Wi-Fi are ready.
    lastMoonAttemptMillis = millis() - MOON_RETRY_MS;

    Serial.println("Setup complete; UI loop running.");
}

void loop()
{
    lv_task_handler();
    serviceIntro();
    serviceWiFi();
    serviceClock();
    serviceMoonFetch();
    delay(5); // yield; not a network wait
}

bool timeIsValid()
{
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    return timeinfo.tm_year >= (2023 - 1900);
}

void serviceWiFi()
{
    uint32_t now = millis();
    wl_status_t status = WiFi.status();

    if (status == WL_CONNECTED) {
        if (wifiState != WifiState::CONNECTED) {
            wifiState = WifiState::CONNECTED;
            Serial.print("Wi-Fi connected. IP: ");
            Serial.println(WiFi.localIP());

            if (!ntpConfigured) {
                configTzTime(TIMEZONE, NTP_SERVER, "time.google.com", "time.cloudflare.com");
                ntpConfigured = true;
                Serial.println("NTP configured; sync will complete in background.");
            }
        }
        return;
    }

    if (wifiState == WifiState::CONNECTED) {
        Serial.println("Wi-Fi connection lost; will reconnect in background.");
        wifiState = WifiState::IDLE;
        wifiAttemptMillis = now - WIFI_RETRY_MS;
    }

    if (wifiState == WifiState::CONNECTING) {
        if (now - wifiAttemptMillis >= WIFI_CONNECT_TIMEOUT_MS) {
            Serial.println("Wi-Fi connect timeout; UI continues. Will retry later.");
            WiFi.disconnect();
            wifiState = WifiState::IDLE;
            wifiAttemptMillis = now;
        }
        return;
    }

    if (now - wifiAttemptMillis >= WIFI_RETRY_MS) {
        Serial.println("Retrying Wi-Fi in background.");
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
        wifiAttemptMillis = now;
        wifiState = WifiState::CONNECTING;
    }
}

void serviceClock()
{
    uint32_t now = millis();
    if (now - lastClockMillis < CLOCK_UPDATE_MS) return;
    lastClockMillis = now;

    bool valid = timeIsValid();
    if (valid && !timeWasValid) {
        Serial.println("NTP time is valid.");
        timeWasValid = true;
        // Permit an immediate moon fetch after initial time sync.
        lastMoonAttemptMillis = now - MOON_RETRY_MS;
    }
    if (valid) updateClockLabels();
}

void serviceIntro()
{
    if (!introActive) return;

    uint32_t now = millis();
    if (now - introFrameMillis < INTRO_FRAME_MS) return;

    introFrameMillis = now;

    setMoonImage(introFrame++);

    // Render this animation frame immediately.
    lv_refr_now(nullptr);

    if (introFrame >= 30) {
        introActive = false;
        Serial.println("Moon intro animation complete.");
    }
}

void serviceMoonFetch()
{
    // Collect worker result on main task; LVGL calls remain here only.
   if (moonResultQueue) {

    MoonResult result;

    if (xQueueReceive(moonResultQueue, &result, 0) == pdTRUE) {

        moonTaskHandle = nullptr;

        if (result.success && !introActive) {

            Serial.printf(
                "Moon age: %.3f days\n",
                result.age
            );

            String phaseName =
                getMoonPhase(result.age);

            int moonIndex =
                getMoonImageIndex(result.age);

            Serial.printf(
                "Moon phase: %s, frame: %d\n",
                phaseName.c_str(),
                moonIndex
            );

            lv_label_set_text(
                ui_phase,
                phaseName.c_str()
            );

            lv_obj_invalidate(ui_phase);

            setMoonImage(moonIndex);

            // Force LVGL to render the new
            // phase label and moon image.
            lv_refr_now(nullptr);

            moonDataValid = true;

        } else {

            Serial.printf(
                "Moon fetch failed, HTTP code: %d; will retry.\n",
                result.httpCode
            );
        }
    }
}




    if (moonTaskHandle != nullptr || !moonResultQueue) return;
  //  if (introActive) return;  // Let startup animation finish first
    if (WiFi.status() != WL_CONNECTED || !timeIsValid()) return;

    uint32_t now = millis();
    uint32_t interval = moonDataValid ? MOON_UPDATE_MS : MOON_RETRY_MS;
    if (now - lastMoonAttemptMillis < interval) return;
    lastMoonAttemptMillis = now;

    BaseType_t ok = xTaskCreate(
        moonFetchTask,
        "moonFetch",
        8192,
        nullptr,
        1,
        &moonTaskHandle
    );

    if (ok != pdPASS) {
        moonTaskHandle = nullptr;
        Serial.println("Could not start moon fetch worker.");
    }
}

void moonFetchTask(void *parameter)
{
    MoonResult result{false, 0.0, -1};

    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    char dateStr[25];
    if (strftime(dateStr, sizeof(dateStr), "%Y-%m-%dT%H:%M", &timeinfo) != 0) {
        String url = "https://svs.gsfc.nasa.gov/api/dialamoon/";
        url += dateStr;
        Serial.print("Background moon fetch: ");
        Serial.println(url);

        WiFiClientSecure client;
        client.setInsecure(); // retained from upstream; production can pin/verify CA later
        client.setTimeout(10000);

        HTTPClient http;
        http.setTimeout(10000);
        if (http.begin(client, url)) {
            result.httpCode = http.GET();
            if (result.httpCode == HTTP_CODE_OK) {
                String payload = http.getString();
                DynamicJsonDocument doc(2048);
                DeserializationError err = deserializeJson(doc, payload);
                if (!err && doc["age"].is<double>()) {
                    result.age = doc["age"].as<double>();
                    result.success = true;
                } else if (err) {
                    Serial.print("JSON parse failed: ");
                    Serial.println(err.c_str());
                }
            }
            http.end();
        }
    }

    if (moonResultQueue) xQueueOverwrite(moonResultQueue, &result);
    vTaskDelete(nullptr);
}

void updateClockLabels()
{
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    char timeStr[9];
    snprintf(timeStr, sizeof(timeStr), "%02d:%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);

    char dateStr[11];
    strftime(dateStr, sizeof(dateStr), "%d %b %y", &timeinfo);

    lv_label_set_text(ui_time, timeStr);
    lv_label_set_text(ui_date, dateStr);
    lv_obj_invalidate(ui_time);
    lv_obj_invalidate(ui_date);
}

void setMoonImage(int index)
{
    if (index < 0) index = 0;
    if (index > 29) index = 29;
    lv_img_set_src(ui_img_moon, moon_frames[index]);
    lv_obj_invalidate(ui_img_moon);
}

String getMoonPhase(double age)
{
    if (age < 1.84566) return "New Moon";
    if (age < 5.53699) return "Waxing Crescent";
    if (age < 9.22831) return "First Quarter";
    if (age < 12.91963) return "Waxing Gibbous";
    if (age < 16.61096) return "Full Moon";
    if (age < 20.30228) return "Waning Gibbous";
    if (age < 23.99361) return "Last Quarter";
    return "Waning Crescent";
}

int getMoonImageIndex(double age)
{
    int idx = (int)(age / 29.53 * 30.0);
    if (idx < 0) idx = 0;
    if (idx > 29) idx = 29;
    return idx;
}
