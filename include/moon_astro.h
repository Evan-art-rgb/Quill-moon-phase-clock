#ifndef MOON_ASTRO_H
#define MOON_ASTRO_H

#include <Arduino.h>
#include <time.h>

struct MoonRiseSet {
    bool riseValid;
    bool setValid;

    time_t riseTime;
    time_t setTime;
};

// Calculate Moon rise/set for the local calendar day containing localTime.
//
// latitude:
//   North positive, South negative.
//
// longitude:
//   East positive, West negative.
//
// localTime:
//   Any valid time on the required local calendar day.
//
// Returned rise/set values are Unix timestamps and can therefore
// be converted to local time with localtime_r().
MoonRiseSet calculateMoonRiseSet(
    time_t localTime,
    double latitude,
    double longitude
);

#endif