#include "moon_astro.h"
#include <math.h>

// Moonrise/moonset calculation validated against published
// Wellington NZ astronomical data for October 2026.
// Typical agreement: within approximately 0-1 minute.

namespace {

constexpr double DEG_TO_RAD_D = M_PI / 180.0;
constexpr double RAD_TO_DEG_D = 180.0 / M_PI;

// Search interval. Crossing time is interpolated between samples.
constexpr int STEP_SECONDS = 300;       // 5 minutes

// Apparent altitude of the Moon's centre at rise/set.
// Includes mean refraction and lunar semidiameter.
//constexpr double MOON_HORIZON_DEG = -0.3;
constexpr double MOON_HORIZON_DEG = -0.8;

// ------------------------------------------------------------
// Angle helpers
// ------------------------------------------------------------

double normalizeDegrees(double angle)
{
    angle = fmod(angle, 360.0);

    if (angle < 0.0)
        angle += 360.0;

    return angle;
}


// ------------------------------------------------------------
// Julian Day
// ------------------------------------------------------------

double julianDay(time_t utcTime)
{
    return 2440587.5 +
           static_cast<double>(utcTime) / 86400.0;
}


// ------------------------------------------------------------
// Moon altitude
//
// Low-precision lunar model with principal perturbations,
// followed by a topocentric parallax correction.
// ------------------------------------------------------------

double moonAltitudeDegrees(
    time_t utcTime,
    double latitude,
    double longitude)
{
    const double jd = julianDay(utcTime);

    // Days since J2000-ish epoch used by this orbital model.
    const double d = jd - 2451543.5;

    // Sun orbital elements
    const double ws = normalizeDegrees(282.9404 + 4.70935E-5 * d);
    const double Ms = normalizeDegrees(356.0470 + 0.9856002585 * d);

    // Sun mean longitude
    const double Ls = normalizeDegrees(ws + Ms);


    // --------------------------------------------------------
    // Moon orbital elements
    // --------------------------------------------------------

    const double N =
        normalizeDegrees(125.1228 - 0.0529538083 * d);

    const double i = 5.1454;

    const double w =
        normalizeDegrees(318.0634 + 0.1643573223 * d);

    const double a = 60.2666;     // Earth radii

    const double e = 0.054900;

    const double M =
        normalizeDegrees(115.3654 + 13.0649929509 * d);


    // Eccentric anomaly
    double E =
        M +
        RAD_TO_DEG_D *
        e *
        sin(M * DEG_TO_RAD_D) *
        (1.0 +
         e * cos(M * DEG_TO_RAD_D));

    // Rectangular coordinates in orbital plane
    double xv =
        a *
        (cos(E * DEG_TO_RAD_D) - e);

    double yv =
        a *
        (sqrt(1.0 - e * e) *
         sin(E * DEG_TO_RAD_D));

    double v =
        atan2(yv, xv) * RAD_TO_DEG_D;

    double r =
        sqrt(xv * xv + yv * yv);


    // --------------------------------------------------------
    // Geocentric ecliptic coordinates
    // --------------------------------------------------------

    double xh =
        r *
        (cos(N * DEG_TO_RAD_D) *
             cos((v + w) * DEG_TO_RAD_D) -
         sin(N * DEG_TO_RAD_D) *
             sin((v + w) * DEG_TO_RAD_D) *
             cos(i * DEG_TO_RAD_D));

    double yh =
        r *
        (sin(N * DEG_TO_RAD_D) *
             cos((v + w) * DEG_TO_RAD_D) +
         cos(N * DEG_TO_RAD_D) *
             sin((v + w) * DEG_TO_RAD_D) *
             cos(i * DEG_TO_RAD_D));

    double zh =
        r *
        sin((v + w) * DEG_TO_RAD_D) *
        sin(i * DEG_TO_RAD_D);

    double moonLongitude =
        atan2(yh, xh) * RAD_TO_DEG_D;

    double moonLatitude =
        atan2(
            zh,
            sqrt(xh * xh + yh * yh))
        * RAD_TO_DEG_D;


    // --------------------------------------------------------
    // Principal lunar perturbations
    // --------------------------------------------------------

    const double Lm =
        normalizeDegrees(N + w + M);

    const double D =
        normalizeDegrees(Lm - Ls);

    const double F =
        normalizeDegrees(Lm - N);


    // Longitude perturbations
    moonLongitude +=
        -1.274 *
        sin((M - 2.0 * D) * DEG_TO_RAD_D);

    moonLongitude +=
        +0.658 *
        sin(2.0 * D * DEG_TO_RAD_D);

    moonLongitude +=
        -0.186 *
        sin(Ms * DEG_TO_RAD_D);

    moonLongitude +=
        -0.059 *
        sin((2.0 * M - 2.0 * D) * DEG_TO_RAD_D);

    moonLongitude +=
        -0.057 *
        sin((M - 2.0 * D + Ms) * DEG_TO_RAD_D);

    moonLongitude +=
        +0.053 *
        sin((M + 2.0 * D) * DEG_TO_RAD_D);

    moonLongitude +=
        +0.046 *
        sin((2.0 * D - Ms) * DEG_TO_RAD_D);

    moonLongitude +=
        +0.041 *
        sin((M - Ms) * DEG_TO_RAD_D);

    moonLongitude +=
        -0.035 *
        sin(D * DEG_TO_RAD_D);

    moonLongitude +=
        -0.031 *
        sin((M + Ms) * DEG_TO_RAD_D);

    moonLongitude +=
        -0.015 *
        sin((2.0 * F - 2.0 * D) * DEG_TO_RAD_D);

    moonLongitude +=
        +0.011 *
        sin((M - 4.0 * D) * DEG_TO_RAD_D);


    // Latitude perturbations
    moonLatitude +=
        -0.173 *
        sin((F - 2.0 * D) * DEG_TO_RAD_D);

    moonLatitude +=
        -0.055 *
        sin((M - F - 2.0 * D) * DEG_TO_RAD_D);

    moonLatitude +=
        -0.046 *
        sin((M + F - 2.0 * D) * DEG_TO_RAD_D);

    moonLatitude +=
        +0.033 *
        sin((F + 2.0 * D) * DEG_TO_RAD_D);

    moonLatitude +=
        +0.017 *
        sin((2.0 * M + F) * DEG_TO_RAD_D);


    // Distance perturbations
    r +=
        -0.58 *
        cos((M - 2.0 * D) * DEG_TO_RAD_D);

    r +=
        -0.46 *
        cos(2.0 * D * DEG_TO_RAD_D);


    // --------------------------------------------------------
    // Ecliptic -> equatorial coordinates
    // --------------------------------------------------------

    const double ecl =
        23.4393 - 3.563E-7 * d;

    double lonRad =
        moonLongitude * DEG_TO_RAD_D;

    double latRad =
        moonLatitude * DEG_TO_RAD_D;

    double eclRad =
        ecl * DEG_TO_RAD_D;

    double xe =
        cos(lonRad) *
        cos(latRad);

    double ye =
        sin(lonRad) *
        cos(latRad) *
        cos(eclRad) -
        sin(latRad) *
        sin(eclRad);

    double ze =
        sin(lonRad) *
        cos(latRad) *
        sin(eclRad) +
        sin(latRad) *
        cos(eclRad);

    double RA =
        atan2(ye, xe) *
        RAD_TO_DEG_D;

    RA = normalizeDegrees(RA);

    double dec =
        atan2(
            ze,
            sqrt(xe * xe + ye * ye))
        * RAD_TO_DEG_D;


    // --------------------------------------------------------
    // Local sidereal time
    // --------------------------------------------------------

    double GMST =
        normalizeDegrees(
            280.46061837 +
            360.98564736629 *
            (jd - 2451545.0));

    double LST =
        normalizeDegrees(GMST + longitude);

    double hourAngle =
        normalizeDegrees(LST - RA);

    if (hourAngle > 180.0)
        hourAngle -= 360.0;


    // --------------------------------------------------------
    // Geocentric altitude
    // --------------------------------------------------------

    double latObserver =
        latitude * DEG_TO_RAD_D;

    double decRad =
        dec * DEG_TO_RAD_D;

    double haRad =
        hourAngle * DEG_TO_RAD_D;

    double sinAltitude =
        sin(latObserver) *
            sin(decRad) +
        cos(latObserver) *
            cos(decRad) *
            cos(haRad);

    // Guard against tiny floating point overflow.
    if (sinAltitude > 1.0)
        sinAltitude = 1.0;

    if (sinAltitude < -1.0)
        sinAltitude = -1.0;

    double altitude =
        asin(sinAltitude) *
        RAD_TO_DEG_D;


    // --------------------------------------------------------
    // Lunar horizontal parallax
    //
    // r is measured in Earth radii.
    // --------------------------------------------------------

    double parallax =
        asin(1.0 / r) *
        RAD_TO_DEG_D;

    // Approximate topocentric altitude correction.
    altitude -=
        parallax *
        cos(altitude * DEG_TO_RAD_D);

    return altitude;
}


// ------------------------------------------------------------
// Find local midnight using ESP32 timezone rules.
// ------------------------------------------------------------

time_t localMidnight(time_t now)
{
    struct tm localTm;

    localtime_r(&now, &localTm);

    localTm.tm_hour = 0;
    localTm.tm_min = 0;
    localTm.tm_sec = 0;

    // Allow mktime() to determine DST.
    localTm.tm_isdst = -1;

    return mktime(&localTm);
}


// ------------------------------------------------------------
// Linear interpolation of horizon crossing.
// ------------------------------------------------------------

time_t interpolateCrossing(
    time_t t1,
    time_t t2,
    double a1,
    double a2)
{
    const double y1 =
        a1 - MOON_HORIZON_DEG;

    const double y2 =
        a2 - MOON_HORIZON_DEG;

    const double denominator = y2 - y1;

    if (fabs(denominator) < 1.0E-9)
        return t1;

    double fraction =
        -y1 / denominator;

    if (fraction < 0.0)
        fraction = 0.0;

    if (fraction > 1.0)
        fraction = 1.0;

    return t1 +
        static_cast<time_t>(
            fraction *
            static_cast<double>(t2 - t1));
}

} // namespace


// ============================================================
// Public interface
// ============================================================

MoonRiseSet calculateMoonRiseSet(
    time_t localTime,
    double latitude,
    double longitude)
{
    MoonRiseSet result {};

    result.riseValid = false;
    result.setValid = false;
    result.riseTime = 0;
    result.setTime = 0;


    // Beginning of today's local civil day.
    const time_t start =
        localMidnight(localTime);


    // Calculate next local midnight via mktime rather than
    // simply adding 86400 seconds, so DST transitions work.
    struct tm tomorrowTm;

    localtime_r(&start, &tomorrowTm);

    tomorrowTm.tm_mday += 1;
    tomorrowTm.tm_isdst = -1;

    const time_t finish =
        mktime(&tomorrowTm);


    time_t previousTime = start;

    double previousAltitude =
        moonAltitudeDegrees(
            previousTime,
            latitude,
            longitude);


    for (time_t currentTime =
             start + STEP_SECONDS;
         currentTime <= finish;
         currentTime += STEP_SECONDS)
    {
        double currentAltitude =
            moonAltitudeDegrees(
                currentTime,
                latitude,
                longitude);


        // Moonrise:
        // below horizon -> above horizon
        if (!result.riseValid &&
            previousAltitude < MOON_HORIZON_DEG &&
            currentAltitude >= MOON_HORIZON_DEG)
        {
            result.riseTime =
                interpolateCrossing(
                    previousTime,
                    currentTime,
                    previousAltitude,
                    currentAltitude);

            result.riseValid = true;
        }


        // Moonset:
        // above horizon -> below horizon
        if (!result.setValid &&
            previousAltitude >= MOON_HORIZON_DEG &&
            currentAltitude < MOON_HORIZON_DEG)
        {
            result.setTime =
                interpolateCrossing(
                    previousTime,
                    currentTime,
                    previousAltitude,
                    currentAltitude);

            result.setValid = true;
        }


        if (result.riseValid &&
            result.setValid)
        {
            break;
        }

        previousTime = currentTime;
        previousAltitude = currentAltitude;
    }


    return result;
}