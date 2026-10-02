/**
 * Quill Moon Clock v1.2 - location development
 * ESP32 WROOM + GC9A01 240x240 + LVGL 8.3.11
 * Based on Moon Phase Clock by nishad2m8:
 * https://github.com/nishad2m8
 *
 * Quill changes:
 *  - Non-blocking Wi-Fi connection/reconnection state machine
 *  - Non-blocking NTP wait (system SNTP runs in background)
 *  - Non-blocking moon intro animation
 *  - NASA HTTPS/JSON work moved off the LVGL loop to a FreeRTOS worker task
 *  - LVGL is touched only by the main task
 *  - Optional observer location
 *  - Local moonrise/moonset calculation
 *  - Temporary moonrise/moonset validation test
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
#include "moon_astro.h"


// ============================================================
// Display
// ============================================================

TFT_eSPI tft = TFT_eSPI();

const char *NTP_SERVER = "pool.ntp.org";

static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf1[240 * 40];
static lv_disp_drv_t disp_drv;


static void disp_flush(
    lv_disp_drv_t *disp,
    const lv_area_t *area,
    lv_color_t *color_p)
{
    uint32_t w = area->x2 - area->x1 + 1;
    uint32_t h = area->y2 - area->y1 + 1;

    tft.startWrite();

    tft.setAddrWindow(
        area->x1,
        area->y1,
        w,
        h
    );

    tft.pushColors(
        (uint16_t *)&color_p->full,
        w * h,
        true
    );

    tft.endWrite();

    lv_disp_flush_ready(disp);
}


// ============================================================
// Moon image frames
// ============================================================

static const lv_img_dsc_t *moon_frames[30] = {

    &ui_img_moon_moon_1_png,
    &ui_img_moon_moon_2_png,
    &ui_img_moon_moon_3_png,
    &ui_img_moon_moon_4_png,
    &ui_img_moon_moon_5_png,
    &ui_img_moon_moon_6_png,
    &ui_img_moon_moon_7_png,
    &ui_img_moon_moon_8_png,
    &ui_img_moon_moon_9_png,
    &ui_img_moon_moon_10_png,

    &ui_img_moon_moon_11_png,
    &ui_img_moon_moon_12_png,
    &ui_img_moon_moon_13_png,
    &ui_img_moon_moon_14_png,
    &ui_img_moon_moon_15_png,
    &ui_img_moon_moon_16_png,
    &ui_img_moon_moon_17_png,
    &ui_img_moon_moon_18_png,
    &ui_img_moon_moon_19_png,
    &ui_img_moon_moon_20_png,

    &ui_img_moon_moon_21_png,
    &ui_img_moon_moon_22_png,
    &ui_img_moon_moon_23_png,
    &ui_img_moon_moon_24_png,
    &ui_img_moon_moon_25_png,
    &ui_img_moon_moon_26_png,
    &ui_img_moon_moon_27_png,
    &ui_img_moon_moon_28_png,
    &ui_img_moon_moon_29_png,
    &ui_img_moon_moon_30_png
};


// ============================================================
// Timing constants
// ============================================================

constexpr uint32_t CLOCK_UPDATE_MS =
    1000;

constexpr uint32_t MOON_UPDATE_MS =
    60UL * 60UL * 1000UL;

constexpr uint32_t MOON_RETRY_MS =
    15UL * 1000UL;

constexpr uint32_t WIFI_RETRY_MS =
    30UL * 1000UL;

constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS =
    20UL * 1000UL;

constexpr uint32_t INTRO_FRAME_MS =
    100;


// ============================================================
// State
// ============================================================

static uint32_t lastClockMillis = 0;
static uint32_t lastMoonAttemptMillis = 0;
static uint32_t wifiAttemptMillis = 0;

static bool moonDataValid = false;
static bool ntpConfigured = false;
static bool timeWasValid = false;

// Track the local calendar day used for the moonrise/moonset calculation.
static int lastMoonRiseSetYear = -1;
static int lastMoonRiseSetYDay = -1;

static MoonRiseSet todayMoonTimes{
    false,
    false,
    0,
    0
};

static lv_obj_t *ui_location = nullptr;
static lv_obj_t *ui_moon_times = nullptr;


enum class WifiState
{
    IDLE,
    CONNECTING,
    CONNECTED
};

static WifiState wifiState =
    WifiState::IDLE;


static bool introActive = true;
static int introFrame = 0;
static uint32_t introFrameMillis = 0;


// ============================================================
// Moon worker result
// ============================================================

struct MoonResult
{
    bool success;
    double age;
    int httpCode;
};

static QueueHandle_t moonResultQueue =
    nullptr;

static TaskHandle_t moonTaskHandle =
    nullptr;


// ============================================================
// Function declarations
// ============================================================

bool timeIsValid();

void serviceWiFi();
void serviceClock();
void serviceMoonRiseSet();
void serviceIntro();
void serviceMoonFetch();

void moonFetchTask(void *parameter);

void updateClockLabels();

void createLocationLabels();
void updateMoonRiseSetDisplay();
void printMoonRiseSet();

   // Validation only — uncomment when testing moonrise/moonset calculations.
   // testMoonRiseSetDates();
//void testMoonRiseSetDates();

String getMoonPhase(double age);

int getMoonImageIndex(double age);

void setMoonImage(int index);


// ============================================================
// Setup
// ============================================================

void setup()
{
    Serial.begin(115200);

    Serial.println();
    Serial.println(
        "Quill Moon Clock v1.2 - location development"
    );


    // --------------------------------------------------------
    // Location configuration
    // --------------------------------------------------------

    if (strlen(LOCATION_NAME) > 0)
    {
        Serial.printf(
            "Location: %s\n",
            LOCATION_NAME
        );
    }


    if (LOCATION_COORDINATES_ENABLED)
    {
        Serial.printf(
            "Observer coordinates: %.4f, %.4f\n",
            LATITUDE,
            LONGITUDE
        );
    }
    else
    {
        Serial.println(
            "Observer coordinates disabled."
        );
    }


    // --------------------------------------------------------
    // TFT
    // --------------------------------------------------------

    tft.begin();

    tft.setRotation(0);

    tft.fillScreen(TFT_BLACK);


    // --------------------------------------------------------
    // LVGL
    // --------------------------------------------------------

    lv_init();

    lv_disp_draw_buf_init(
        &draw_buf,
        buf1,
        nullptr,
        240 * 40
    );

    lv_disp_drv_init(
        &disp_drv
    );

    disp_drv.hor_res = 240;
    disp_drv.ver_res = 240;
    disp_drv.flush_cb = disp_flush;
    disp_drv.draw_buf = &draw_buf;

    lv_disp_drv_register(
        &disp_drv
    );


    ui_init();

    // Add v1.2 location/moonrise/moonset labels outside the
    // SquareLine-generated files so future exports do not overwrite them.
    createLocationLabels();

    lv_task_handler();


    // --------------------------------------------------------
    // Moon worker queue
    // --------------------------------------------------------

    moonResultQueue =
        xQueueCreate(
            1,
            sizeof(MoonResult)
        );

    if (!moonResultQueue)
    {
        Serial.println(
            "ERROR: could not create moon result queue."
        );
    }


    // --------------------------------------------------------
    // Start networking without blocking
    // --------------------------------------------------------

    WiFi.mode(WIFI_STA);

    WiFi.begin(
        WIFI_SSID,
        WIFI_PASSWORD
    );

    wifiAttemptMillis =
        millis();

    wifiState =
        WifiState::CONNECTING;

    Serial.println(
        "Wi-Fi connection started in background."
    );


    // --------------------------------------------------------
    // Initial timing
    // --------------------------------------------------------

    introFrameMillis =
        millis();

    lastClockMillis =
        millis();

    // Allow first moon fetch as soon as
    // time + Wi-Fi are ready.

    lastMoonAttemptMillis =
        millis() - MOON_RETRY_MS;


    Serial.println(
        "Setup complete; UI loop running."
    );
}


// ============================================================
// Main loop
// ============================================================

void loop()
{
    lv_task_handler();

    serviceIntro();
    serviceWiFi();
    serviceClock();
    serviceMoonRiseSet();
    serviceMoonFetch();

    // Yield only - not a network wait.

    delay(5);
}


// ============================================================
// Time validity
// ============================================================

bool timeIsValid()
{
    time_t now;
    struct tm timeinfo;

    time(&now);

    localtime_r(
        &now,
        &timeinfo
    );

    return
        timeinfo.tm_year >=
        (2023 - 1900);
}


// ============================================================
// Wi-Fi service
// ============================================================

void serviceWiFi()
{
    uint32_t now =
        millis();

    wl_status_t status =
        WiFi.status();


    // --------------------------------------------------------
    // Connected
    // --------------------------------------------------------

    if (status == WL_CONNECTED)
    {
        if (wifiState != WifiState::CONNECTED)
        {
            wifiState =
                WifiState::CONNECTED;

            Serial.print(
                "Wi-Fi connected. IP: "
            );

            Serial.println(
                WiFi.localIP()
            );


            if (!ntpConfigured)
            {
                configTzTime(
                    TIMEZONE,
                    NTP_SERVER,
                    "time.google.com",
                    "time.cloudflare.com"
                );

                ntpConfigured = true;

                Serial.println(
                    "NTP configured; sync will complete in background."
                );
            }
        }

        return;
    }


    // --------------------------------------------------------
    // Connection lost
    // --------------------------------------------------------

    if (wifiState == WifiState::CONNECTED)
    {
        Serial.println(
            "Wi-Fi connection lost; will reconnect in background."
        );

        wifiState =
            WifiState::IDLE;

        wifiAttemptMillis =
            now - WIFI_RETRY_MS;
    }


    // --------------------------------------------------------
    // Connecting
    // --------------------------------------------------------

    if (wifiState == WifiState::CONNECTING)
    {
        if (
            now - wifiAttemptMillis >=
            WIFI_CONNECT_TIMEOUT_MS
        )
        {
            Serial.println(
                "Wi-Fi connect timeout; UI continues. Will retry later."
            );

            WiFi.disconnect();

            wifiState =
                WifiState::IDLE;

            wifiAttemptMillis =
                now;
        }

        return;
    }


    // --------------------------------------------------------
    // Retry
    // --------------------------------------------------------

    if (
        now - wifiAttemptMillis >=
        WIFI_RETRY_MS
    )
    {
        Serial.println(
            "Retrying Wi-Fi in background."
        );

        WiFi.begin(
            WIFI_SSID,
            WIFI_PASSWORD
        );

        wifiAttemptMillis =
            now;

        wifiState =
            WifiState::CONNECTING;
    }
}


// ============================================================
// Clock service
// ============================================================

void serviceClock()
{
    uint32_t now =
        millis();


    if (
        now - lastClockMillis <
        CLOCK_UPDATE_MS
    )
    {
        return;
    }


    lastClockMillis =
        now;


    bool valid =
        timeIsValid();


    // --------------------------------------------------------
    // First valid NTP time
    // --------------------------------------------------------

    if (
        valid &&
        !timeWasValid
    )
    {
        Serial.println(
            "NTP time is valid."
        );

        timeWasValid = true;


        // Moonrise/moonset is handled by serviceMoonRiseSet(),
        // which calculates immediately for the current local day.


        // TEMPORARY v1.2 validation routine.
        // Validation only — uncomment when testing moonrise/moonset calculations.
        //testMoonRiseSetDates();


        // Permit an immediate NASA moon fetch
        // after initial time sync.

        lastMoonAttemptMillis =
            now - MOON_RETRY_MS;
    }


    if (valid)
    {
        updateClockLabels();
    }
}


// ============================================================
// Moonrise / moonset daily service
// ============================================================

void serviceMoonRiseSet()
{
    if (!timeWasValid || !LOCATION_COORDINATES_ENABLED)
    {
        return;
    }

    time_t now;
    time(&now);

    struct tm localTm;
    localtime_r(
        &now,
        &localTm
    );

    const int year =
        localTm.tm_year + 1900;

    const int yday =
        localTm.tm_yday;


    // Nothing to do if today's values have already been calculated.

    if (
        year == lastMoonRiseSetYear &&
        yday == lastMoonRiseSetYDay
    )
    {
        return;
    }


    lastMoonRiseSetYear =
        year;

    lastMoonRiseSetYDay =
        yday;


    Serial.printf(
        "\nLocal date changed: %04d-%02d-%02d\n",
        year,
        localTm.tm_mon + 1,
        localTm.tm_mday
    );


    todayMoonTimes =
        calculateMoonRiseSet(
            now,
            LATITUDE,
            LONGITUDE
        );

    printMoonRiseSet();
    updateMoonRiseSetDisplay();
}


// ============================================================
// Intro animation
// ============================================================

void serviceIntro()
{
    if (!introActive)
    {
        return;
    }


    uint32_t now =
        millis();


    if (
        now - introFrameMillis <
        INTRO_FRAME_MS
    )
    {
        return;
    }


    introFrameMillis =
        now;


    setMoonImage(
        introFrame++
    );


    // Render this animation frame immediately.

    lv_refr_now(nullptr);


    if (introFrame >= 30)
    {
        introActive = false;

        Serial.println(
            "Moon intro animation complete."
        );
    }
}


// ============================================================
// Moon fetch service
// ============================================================

void serviceMoonFetch()
{
    // --------------------------------------------------------
    // Collect worker result.
    //
    // LVGL calls remain on the main task only.
    // --------------------------------------------------------

    if (moonResultQueue)
    {
        MoonResult result;


        if (
            xQueueReceive(
                moonResultQueue,
                &result,
                0
            ) == pdTRUE
        )
        {
            moonTaskHandle =
                nullptr;


            if (
                result.success &&
                !introActive
            )
            {
                Serial.printf(
                    "Moon age: %.3f days\n",
                    result.age
                );


                String phaseName =
                    getMoonPhase(
                        result.age
                    );


                int moonIndex =
                    getMoonImageIndex(
                        result.age
                    );


                Serial.printf(
                    "Moon phase: %s, frame: %d\n",
                    phaseName.c_str(),
                    moonIndex
                );


                lv_label_set_text(
                    ui_phase,
                    phaseName.c_str()
                );

                lv_obj_invalidate(
                    ui_phase
                );


                setMoonImage(
                    moonIndex
                );


                // Force LVGL to render the
                // new phase label and moon image.

                lv_refr_now(nullptr);


                moonDataValid =
                    true;
            }
            else
            {
                Serial.printf(
                    "Moon fetch failed, HTTP code: %d; will retry.\n",
                    result.httpCode
                );
            }
        }
    }


    // --------------------------------------------------------
    // Don't start another worker if one is running.
    // --------------------------------------------------------

    if (
        moonTaskHandle != nullptr ||
        !moonResultQueue
    )
    {
        return;
    }


    // Retain existing v1.1 behaviour:
    // moon fetch may begin while intro is running.

    // if (introActive) return;


    if (
        WiFi.status() != WL_CONNECTED ||
        !timeIsValid()
    )
    {
        return;
    }


    uint32_t now =
        millis();


    uint32_t interval =
        moonDataValid
            ? MOON_UPDATE_MS
            : MOON_RETRY_MS;


    if (
        now - lastMoonAttemptMillis <
        interval
    )
    {
        return;
    }


    lastMoonAttemptMillis =
        now;


    BaseType_t ok =
        xTaskCreate(
            moonFetchTask,
            "moonFetch",
            8192,
            nullptr,
            1,
            &moonTaskHandle
        );


    if (ok != pdPASS)
    {
        moonTaskHandle =
            nullptr;

        Serial.println(
            "Could not start moon fetch worker."
        );
    }
}


// ============================================================
// NASA moon fetch worker
// ============================================================

void moonFetchTask(void *parameter)
{
    MoonResult result{
        false,
        0.0,
        -1
    };


    time_t now;

    struct tm timeinfo;


    time(&now);

    localtime_r(
        &now,
        &timeinfo
    );


    char dateStr[25];


    if (
        strftime(
            dateStr,
            sizeof(dateStr),
            "%Y-%m-%dT%H:%M",
            &timeinfo
        ) != 0
    )
    {
        String url =
            "https://svs.gsfc.nasa.gov/api/dialamoon/";

        url +=
            dateStr;


        Serial.print(
            "Background moon fetch: "
        );

        Serial.println(
            url
        );


        WiFiClientSecure client;

        // Retained from upstream.
        // Production version can verify/pin CA later.

        client.setInsecure();

        client.setTimeout(
            10000
        );


        HTTPClient http;

        http.setTimeout(
            10000
        );


        if (
            http.begin(
                client,
                url
            )
        )
        {
            result.httpCode =
                http.GET();


            if (
                result.httpCode ==
                HTTP_CODE_OK
            )
            {
                String payload =
                    http.getString();


                DynamicJsonDocument doc(
                    2048
                );


                DeserializationError err =
                    deserializeJson(
                        doc,
                        payload
                    );


                if (
                    !err &&
                    doc["age"].is<double>()
                )
                {
                    result.age =
                        doc["age"].as<double>();

                    result.success =
                        true;
                }
                else if (err)
                {
                    Serial.print(
                        "JSON parse failed: "
                    );

                    Serial.println(
                        err.c_str()
                    );
                }
            }


            http.end();
        }
    }


    if (moonResultQueue)
    {
        xQueueOverwrite(
            moonResultQueue,
            &result
        );
    }


    vTaskDelete(
        nullptr
    );
}


// ============================================================
// Clock labels
// ============================================================

void updateClockLabels()
{
    time_t now;

    struct tm timeinfo;


    time(&now);

    localtime_r(
        &now,
        &timeinfo
    );


    char timeStr[9];

    snprintf(
        timeStr,
        sizeof(timeStr),
        "%02d:%02d:%02d",
        timeinfo.tm_hour,
        timeinfo.tm_min,
        timeinfo.tm_sec
    );


    char dateStr[11];

    strftime(
        dateStr,
        sizeof(dateStr),
        "%d %b %y",
        &timeinfo
    );


    lv_label_set_text(
        ui_time,
        timeStr
    );

    lv_label_set_text(
        ui_date,
        dateStr
    );


    lv_obj_invalidate(
        ui_time
    );

    lv_obj_invalidate(
        ui_date
    );
}


// ============================================================
// Moon image
// ============================================================

void setMoonImage(int index)
{
    if (index < 0)
    {
        index = 0;
    }


    if (index > 29)
    {
        index = 29;
    }


    lv_img_set_src(
        ui_img_moon,
        moon_frames[index]
    );


    lv_obj_invalidate(
        ui_img_moon
    );
}


// ============================================================
// Moon phase name
// ============================================================

String getMoonPhase(double age)
{
    if (age < 1.84566)
        return "New Moon";

    if (age < 5.53699)
        return "Waxing Crescent";

    if (age < 9.22831)
        return "First Quarter";

    if (age < 12.91963)
        return "Waxing Gibbous";

    if (age < 16.61096)
        return "Full Moon";

    if (age < 20.30228)
        return "Waning Gibbous";

    if (age < 23.99361)
        return "Last Quarter";

    return "Waning Crescent";
}


// ============================================================
// Moon image index
// ============================================================

int getMoonImageIndex(double age)
{
    int idx =
        (int)(
            age /
            29.53 *
            30.0
        );


    if (idx < 0)
    {
        idx = 0;
    }


    if (idx > 29)
    {
        idx = 29;
    }


    return idx;
}


// ============================================================
// v1.2 location / moonrise / moonset display
// ============================================================

void createLocationLabels()
{
    ui_location =
        lv_label_create(
            ui_Screen1
        );

    // Set the maximum width available for the location label.
    // Longer names are truncated with an ellipsis to prevent
    // overlap with the moonrise/moonset times.

    lv_obj_set_width(
        ui_location,
        95
    );

    lv_obj_set_height(
        ui_location,
        LV_SIZE_CONTENT
    );
    // Truncate location with ... if it exceeds the available width
    // Limit the location label to the available display width.
    // Longer location names are automatically truncated with an ellipsis
    // to prevent them overlapping the moonrise/moonset times.
    lv_label_set_long_mode(
        ui_location,
        LV_LABEL_LONG_DOT
    );
    
    lv_obj_align(
        ui_location,
        LV_ALIGN_LEFT_MID,
        30,
        68
    );

    lv_label_set_text(
        ui_location,
        strlen(LOCATION_NAME) > 0
            ? LOCATION_NAME
            : ""
    );

    lv_obj_set_style_text_color(
        ui_location,
        lv_color_hex(0xDBDBDB),
        LV_PART_MAIN | LV_STATE_DEFAULT
    );

    lv_obj_set_style_text_opa(
        ui_location,
        255,
        LV_PART_MAIN | LV_STATE_DEFAULT
    );

    lv_obj_set_style_text_align(
        ui_location,
 //       LV_TEXT_ALIGN_CENTER,
        LV_TEXT_ALIGN_LEFT,
        LV_PART_MAIN | LV_STATE_DEFAULT
    );

    lv_obj_set_style_text_font(
        ui_location,
        &ui_font_datefont,
        LV_PART_MAIN | LV_STATE_DEFAULT
    );


    ui_moon_times =
        lv_label_create(
            ui_Screen1
        );

    lv_obj_set_width(
        ui_moon_times,
        200
    );

    lv_obj_set_height(
        ui_moon_times,
        LV_SIZE_CONTENT
    );

    lv_obj_align(
        ui_moon_times,
        LV_ALIGN_RIGHT_MID,
        -35,
        68
    );

    // Enable LVGL inline recolouring:
    // rise = green, set = red.
    lv_label_set_recolor(
        ui_moon_times,
        true
    );

    lv_label_set_text(
        ui_moon_times,
        "#00FF00 R --:--#   #FF0000 S --:--#"
    );

    lv_obj_set_style_text_color(
        ui_moon_times,
        lv_color_hex(0xDBDBDB),
        LV_PART_MAIN | LV_STATE_DEFAULT
    );

    lv_obj_set_style_text_opa(
        ui_moon_times,
        255,
        LV_PART_MAIN | LV_STATE_DEFAULT
    );

    lv_obj_set_style_text_align(
        ui_moon_times,
        LV_TEXT_ALIGN_RIGHT,
        LV_PART_MAIN | LV_STATE_DEFAULT
    );

    lv_obj_set_style_text_font(
        ui_moon_times,
        &ui_font_datefont,
        LV_PART_MAIN | LV_STATE_DEFAULT
    );


    if (!LOCATION_COORDINATES_ENABLED)
    {
        lv_label_set_text(
            ui_moon_times,
            ""
        );
    }
}


void updateMoonRiseSetDisplay()
{
    if (
        !LOCATION_COORDINATES_ENABLED ||
        !ui_moon_times
    )
    {
        return;
    }


    char riseText[6] = "--:--";
    char setText[6] = "--:--";


    if (todayMoonTimes.riseValid)
    {
        struct tm riseLocal;

        localtime_r(
            &todayMoonTimes.riseTime,
            &riseLocal
        );

        strftime(
            riseText,
            sizeof(riseText),
            "%H:%M",
            &riseLocal
        );
    }


    if (todayMoonTimes.setValid)
    {
        struct tm setLocal;

        localtime_r(
            &todayMoonTimes.setTime,
            &setLocal
        );

        strftime(
            setText,
            sizeof(setText),
            "%H:%M",
            &setLocal
        );
    }


    char moonTimesText[64];

    snprintf(
        moonTimesText,
        sizeof(moonTimesText),
        "#00FF00 R %s#   #FF0000 S %s#",
        riseText,
        setText
    );


    lv_label_set_text(
        ui_moon_times,
        moonTimesText
    );

    lv_obj_invalidate(
        ui_moon_times
    );

    if (ui_location)
    {
        lv_obj_invalidate(
            ui_location
        );
    }
}


// ============================================================
// Today's moonrise / moonset
// ============================================================

void printMoonRiseSet()
{
    if (!LOCATION_COORDINATES_ENABLED)
    {
        return;
    }


    if (!timeIsValid())
    {
        return;
    }


    const MoonRiseSet &moonTimes =
        todayMoonTimes;


    Serial.println();

    Serial.println(
        "Moon rise/set:"
    );


    // --------------------------------------------------------
    // Rise
    // --------------------------------------------------------

    if (moonTimes.riseValid)
    {
        struct tm riseLocal;

        localtime_r(
            &moonTimes.riseTime,
            &riseLocal
        );


        char riseText[6];

        strftime(
            riseText,
            sizeof(riseText),
            "%H:%M",
            &riseLocal
        );


        Serial.printf(
            "  Rise: %s\n",
            riseText
        );
    }
    else
    {
        Serial.println(
            "  Rise: --:--"
        );
    }


    // --------------------------------------------------------
    // Set
    // --------------------------------------------------------

    if (moonTimes.setValid)
    {
        struct tm setLocal;

        localtime_r(
            &moonTimes.setTime,
            &setLocal
        );


        char setText[6];

        strftime(
            setText,
            sizeof(setText),
            "%H:%M",
            &setLocal
        );


        Serial.printf(
            "  Set : %s\n",
            setText
        );
    }
    else
    {
        Serial.println(
            "  Set : --:--"
        );
    }


    Serial.println();
}


// ============================================================
// TEMPORARY v1.2 astronomy validation
// ============================================================

/* void testMoonRiseSetDates()
{
    if (!LOCATION_COORDINATES_ENABLED)
    {
        return;
    }


    struct TestDate
    {
        int year;
        int month;
        int day;
    };


    const TestDate tests[] =
    {
        {2026, 10, 2},
    {2026, 10, 3},
    {2026, 10, 10},
    {2026, 10, 20},
    {2026, 10, 29},
    {2026, 10, 30},
    {2026, 10, 31}
    };


    Serial.println(
        "================================"
    );

    Serial.println(
        "Moon rise/set validation"
    );

    Serial.println(
        "================================"
    );


    for (const TestDate &test : tests)
    {
        struct tm localDate = {};


        localDate.tm_year =
            test.year - 1900;

        localDate.tm_mon =
            test.month - 1;

        localDate.tm_mday =
            test.day;


        // Midday avoids ambiguity around
        // midnight and DST transitions.

        localDate.tm_hour =
            12;

        localDate.tm_min =
            0;

        localDate.tm_sec =
            0;

        localDate.tm_isdst =
            -1;


        time_t testTime =
            mktime(
                &localDate
            );


        MoonRiseSet result =
            calculateMoonRiseSet(
                testTime,
                LATITUDE,
                LONGITUDE
            );


        Serial.printf(
            "%04d-%02d-%02d  ",
            test.year,
            test.month,
            test.day
        );


        // ----------------------------------------------------
        // Rise
        // ----------------------------------------------------

        if (result.riseValid)
        {
            struct tm riseLocal;

            localtime_r(
                &result.riseTime,
                &riseLocal
            );


            char riseText[6];

            strftime(
                riseText,
                sizeof(riseText),
                "%H:%M",
                &riseLocal
            );


            Serial.printf(
                "Rise %s  ",
                riseText
            );
        }
        else
        {
            Serial.print(
                "Rise --:--  "
            );
        }


        // ----------------------------------------------------
        // Set
        // ----------------------------------------------------

        if (result.setValid)
        {
            struct tm setLocal;

            localtime_r(
                &result.setTime,
                &setLocal
            );


            char setText[6];

            strftime(
                setText,
                sizeof(setText),
                "%H:%M",
                &setLocal
            );


            Serial.printf(
                "Set %s",
                setText
            );
        }
        else
        {
            Serial.print(
                "Set --:--"
            );
        }


        Serial.println();
    }


    Serial.println(
        "================================"
    );

    Serial.println();
}
    */