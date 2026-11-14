"""Sunrise and sunset: the NOAA solar calculator, accurate to about a minute."""

import math
from datetime import UTC, date, datetime, timedelta

# The sun's upper limb on the horizon, including standard refraction.
HORIZON = math.radians(90.833)


def sun_times(day: date, latitude: float, longitude: float) -> tuple[datetime, datetime]:
    """Sunrise and sunset (UTC) on a local date; longitude is positive east.

    In polar night both are at solar noon and under the midnight sun twelve
    hours before and after it, so daylight lasts 0 or 24 hours.
    """
    midnight = datetime(day.year, day.month, day.day, tzinfo=UTC)
    noon = midnight + timedelta(hours=12 - longitude / 15)
    # Julian centuries since J2000, at about local solar noon.
    t = (noon.timestamp() / 86400 + 2440587.5 - 2451545) / 36525
    mean_longitude = math.radians((280.46646 + t * (36000.76983 + t * 0.0003032)) % 360)
    anomaly = math.radians(357.52911 + t * (35999.05029 - 0.0001537 * t))
    eccentricity = 0.016708634 - t * (0.000042037 + 0.0000001267 * t)
    center = (
        math.sin(anomaly) * (1.914602 - t * (0.004817 + 0.000014 * t))
        + math.sin(2 * anomaly) * (0.019993 - 0.000101 * t)
        + math.sin(3 * anomaly) * 0.000289
    )
    node = math.radians(125.04 - 1934.136 * t)
    apparent = mean_longitude + math.radians(center - 0.00569 - 0.00478 * math.sin(node))
    obliquity = math.radians(
        23 + (26 + (21.448 - t * (46.815 + t * (0.00059 - t * 0.001813))) / 60) / 60 + 0.00256 * math.cos(node)
    )
    declination = math.asin(math.sin(obliquity) * math.sin(apparent))
    y = math.tan(obliquity / 2) ** 2
    equation_of_time = 4 * math.degrees(  # minutes
        y * math.sin(2 * mean_longitude)
        - 2 * eccentricity * math.sin(anomaly)
        + 4 * eccentricity * y * math.sin(anomaly) * math.cos(2 * mean_longitude)
        - 0.5 * y * y * math.sin(4 * mean_longitude)
        - 1.25 * eccentricity * eccentricity * math.sin(2 * anomaly)
    )
    phi = math.radians(latitude)
    cos_hour = math.cos(HORIZON) / (math.cos(phi) * math.cos(declination)) - math.tan(phi) * math.tan(declination)
    half_day = 4 * math.degrees(math.acos(min(1.0, max(-1.0, cos_hour))))  # minutes
    transit = midnight + timedelta(minutes=720 - 4 * longitude - equation_of_time)
    return transit - timedelta(minutes=half_day), transit + timedelta(minutes=half_day)
