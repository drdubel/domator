"""Waveshare HMMD mmWave presence sensors (ESP32-C3 SuperMini, switch_role=presence).

A sensor reports presence on three inputs that RCM wires like switch buttons.
Each has a fixed routing type, understood by the mesh root:
  a: outputs follow presence (on while occupied, off when clear)
  b: outputs turn on when presence starts
  c: outputs turn off when the area becomes clear
Range gates, the absence delay and per-gate thresholds are stored in the
radar itself. Turbacz keeps the configuration the sensor last reported.

A schedule pauses a sensor every day, from a time or a sun event to another:
  {"from": {"time": "23:00"}, "to": {"sun": "sunrise", "offset": 30}}
Sensors have no wall clock, so Turbacz sends each one a plan, the pauses of
the next days in seconds from now, and the sensor follows it on its own timer
while Turbacz is unreachable. The sensor reports which plan it follows.
"""

import json
import os
import zlib
from datetime import UTC, datetime, time, timedelta
from zoneinfo import ZoneInfo

from turbacz.location import coordinates, ip_place
from turbacz.settings import config
from turbacz.sun import sun_times

GATES = 16
PRESENCE_BUTTONS = {"a": 2, "b": 3, "c": 4}
CONFIG_KEYS = ("min", "max", "delay", "trigger", "hold", "led")
SUN_EVENTS = ("sunrise", "sunset")
SUN_OFFSET_MAX = 240  # minutes before or after the sun event
PLAN_DAYS = 7
PLAN_MAX = 14 * 86400  # the longest plan the firmware accepts
PLAN_REFRESH = (PLAN_DAYS - 1) * 86400  # renew a plan with less left: daily
# A pause passes midnight when it starts later in the day than it ends, with
# sunrise counted at 06:00 and sunset at 18:00. A pause from sunset to 22:00 is
# skipped on days when the sun sets later instead of lasting until 22:00 the
# next day.
NOMINAL_MINUTE = {"sunrise": 6 * 60, "sunset": 18 * 60}


def report_config(data: dict) -> dict:
    """The cached form of a validated ``{"report": "config"}`` status."""
    return {
        "min": data["min"],
        "max": data["max"],
        "delay": data["delay"],
        "trigger": list(data["trigger"]),
        "hold": list(data["hold"]),
        "led": bool(data["led"]),
        "fw": data.get("fw", ""),
    }


def command_payload(cmd: dict) -> str:
    """Firmware command for a validated RCM ``presence_command``."""
    action = cmd["action"]
    payload = {"radar": action}
    if action == "apply":
        config = cmd["config"]
        payload.update(
            min=int(config["min"]),
            max=int(config["max"]),
            delay=int(config["delay"]),
            trigger=[int(value) for value in config["trigger"]],
            hold=[int(value) for value in config["hold"]],
            led=int(config["led"]),
        )
    elif action in {"calibrate", "live"}:
        payload["seconds"] = int(cmd["seconds"])
    return json.dumps(payload, separators=(",", ":"))


def home_zone():
    """The home's time zone: [location] timezone, else the system's."""
    if config.location.timezone:
        return ZoneInfo(config.location.timezone)
    key = os.environ.get("TZ", "").lstrip(":")
    try:
        if key:
            return ZoneInfo(key)
    except (ValueError, KeyError):
        pass  # a POSIX rule rather than a zone name
    try:
        with open("/etc/localtime", "rb") as file:
            return ZoneInfo.from_file(file, key="localtime")
    except (OSError, ValueError):
        return UTC


def _uses_sun(schedule) -> bool:
    return any("sun" in schedule[key] for key in ("from", "to"))


def applicable(schedule) -> bool:
    """Whether Turbacz can follow a schedule: sun events need the coordinates."""
    if schedule is None:
        return False
    return coordinates() is not None or not _uses_sun(schedule)


def _clock(boundary) -> tuple[int, int]:
    hours, minutes = boundary["time"].split(":")
    return int(hours), int(minutes)


def _nominal_minute(boundary) -> int:
    if "time" in boundary:
        hours, minutes = _clock(boundary)
        return hours * 60 + minutes
    return (NOMINAL_MINUTE[boundary["sun"]] + boundary["offset"]) % 1440


def _moment(boundary, day, zone) -> datetime:
    # In UTC: datetimes sharing a ZoneInfo compare and subtract as wall times.
    if "time" in boundary:
        return datetime.combine(day, time(*_clock(boundary)), zone).astimezone(UTC)
    sunrise, sunset = sun_times(day, *coordinates())
    return (sunrise if boundary["sun"] == "sunrise" else sunset) + timedelta(minutes=boundary["offset"])


def paused_periods(schedule, start: datetime, end: datetime) -> list[tuple[datetime, datetime]]:
    """The pauses of an applicable schedule that overlap [start, end), clipped and merged, in UTC."""
    zone = home_zone()
    start, end = start.astimezone(UTC), end.astimezone(UTC)
    overnight = _nominal_minute(schedule["from"]) > _nominal_minute(schedule["to"])
    periods = []
    # A pause that began yesterday may still last.
    day = start.astimezone(zone).date() - timedelta(days=1)
    while day <= end.astimezone(zone).date():
        begin = max(_moment(schedule["from"], day, zone), start)
        finish = min(_moment(schedule["to"], day + timedelta(days=1 if overnight else 0), zone), end)
        if begin < finish:
            if periods and begin <= periods[-1][1]:
                periods[-1] = (periods[-1][0], max(periods[-1][1], finish))
            else:
                periods.append((begin, finish))
        day += timedelta(days=1)
    return periods


def plan_id(schedule) -> int:
    """Identifies the plan of a schedule, 0 for none. Sensors report it back."""
    if not applicable(schedule):
        return 0
    # A new location changes only plans that follow the sun.
    home = coordinates() if _uses_sun(schedule) else None
    key = json.dumps([schedule, home, config.location.timezone], sort_keys=True)
    return zlib.crc32(key.encode()) or 1


def plan_payload(schedule, now: datetime) -> str:
    """Firmware command with the pauses of the next PLAN_DAYS, in seconds from now."""
    identifier = plan_id(schedule)
    length = PLAN_DAYS * 86400 if identifier else 0
    off = []
    if identifier:
        for begin, finish in paused_periods(schedule, now, now + timedelta(seconds=length)):
            window = [int((begin - now).total_seconds()), int((finish - now).total_seconds())]
            if window[0] < window[1]:
                off.append(window)
    return json.dumps({"radar": "schedule", "id": identifier, "for": length, "off": off}, separators=(",", ":"))


def sun_today(now: datetime) -> dict:
    """The home's time zone, today's sunrise and sunset there and, when found
    from the IP address, its town, for RCM."""
    zone = home_zone()
    local = now.astimezone(zone)
    today = {"zone": local.tzname(), "sunrise": None, "sunset": None, "ip_place": ip_place()}
    if (home := coordinates()) is not None:
        sunrise, sunset = sun_times(local.date(), *home)
        today["sunrise"] = f"{sunrise.astimezone(zone):%H:%M}"
        today["sunset"] = f"{sunset.astimezone(zone):%H:%M}"
    return today
