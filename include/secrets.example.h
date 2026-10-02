#ifndef SECRETS_H
#define SECRETS_H

// ------------------------------------------------------------
// Wi-Fi configuration
// ------------------------------------------------------------

const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";


// ------------------------------------------------------------
// Time zone
// ------------------------------------------------------------

// New Zealand: NZST / NZDT automatic daylight-saving adjustment
const char* TIMEZONE = "NZST-12NZDT,M9.5.0,M4.1.0/3";


// ------------------------------------------------------------
// Optional observer location
// ------------------------------------------------------------

// Location name displayed on the clock.
// Leave empty ("") to disable location display.
const char* LOCATION_NAME = "";

// Observer coordinates used for Moon rise/set calculations.
//
// Set LOCATION_COORDINATES_ENABLED to true and enter both
// latitude and longitude to enable Moon rise/set.
//
// South latitude = negative
// West longitude = negative
//
// Example only:
// const double LATITUDE  = -41.000000;
// const double LONGITUDE = 175.000000;

// Set to true when valid coordinates have been entered.
// Set to false to disable Moon rise/set calculations.
const bool LOCATION_COORDINATES_ENABLED = false;

const double LATITUDE  = 0.0;
const double LONGITUDE = 0.0;


#endif