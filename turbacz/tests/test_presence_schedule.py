"""Presence sensor schedules: sun times, daily pauses, plans and their delivery."""

import json
from datetime import UTC, date, datetime, timedelta
from time import time
from zoneinfo import ZoneInfo

import pytest
from pydantic import ValidationError
from starlette.websockets import WebSocketDisconnect

from turbacz.authorization import command_capability
from turbacz.mqtt_validation import validate_payload
from turbacz.presence import PLAN_DAYS, PLAN_REFRESH, home_zone, paused_periods, plan_id, plan_payload, sun_today
from turbacz.security import RateLimiter
from turbacz.settings import LocationSettings, config
from turbacz.sun import sun_times
from turbacz.validation import validate_command

from .test_presence import SENSOR, heartbeat
from .test_presence import broker_env as broker_env
from .test_security import application as application

KRAKOW = LocationSettings(latitude=50.06, longitude=19.94, timezone="Europe/Warsaw")
NIGHT = {"from": {"time": "23:00"}, "to": {"time": "06:00"}}
DAYLIGHT = {"from": {"sun": "sunrise", "offset": 0}, "to": {"sun": "sunset", "offset": 0}}


@pytest.fixture
def krakow(monkeypatch):
    monkeypatch.setattr(config, "location", KRAKOW)


def at(text, location=KRAKOW):
    """A wall time at a location, e.g. '2026-03-28 23:00'."""
    return datetime.fromisoformat(text).replace(tzinfo=ZoneInfo(location.timezone))


# Reference times from sunrise-sunset.org.
@pytest.mark.parametrize("place,day,sunrise,sunset", [
    ((52.2297, 21.0122), date(2026, 6, 21), "2026-06-21T02:14:21", "2026-06-21T19:01:10"),  # Warsaw
    ((52.2297, 21.0122), date(2026, 12, 21), "2026-12-21T06:42:47", "2026-12-21T14:25:11"),
    ((38.8895, -77.0353), date(2026, 6, 21), "2026-06-21T09:43:04", "2026-06-22T00:36:57"),  # Washington
    ((-33.8688, 151.2093), date(2026, 11, 14), "2026-11-13T18:44:07", "2026-11-14T08:34:55"),  # Sydney
])
def test_sun_times_match_published_tables(place, day, sunrise, sunset):
    rise, fall = sun_times(day, *place)
    assert abs(rise - datetime.fromisoformat(sunrise).replace(tzinfo=UTC)) < timedelta(minutes=1)
    assert abs(fall - datetime.fromisoformat(sunset).replace(tzinfo=UTC)) < timedelta(minutes=1)


def test_polar_night_has_no_daylight_and_the_midnight_sun_no_night():
    rise, fall = sun_times(date(2026, 12, 21), 69.65, 18.96)  # Tromsø
    assert rise == fall
    rise, fall = sun_times(date(2026, 6, 21), 69.65, 18.96)
    assert fall - rise == timedelta(hours=24)


@pytest.mark.parametrize("invalid", [
    {"latitude": 50.06}, {"longitude": 19.94}, {"latitude": 90, "longitude": 0},
    {"latitude": 50, "longitude": 181}, {"timezone": "Europe/Atlantis"}, {"timezone": "../etc/passwd"},
])
def test_location_needs_both_coordinates_and_a_known_time_zone(invalid):
    with pytest.raises(ValidationError):
        LocationSettings(**invalid)


def test_home_zone_is_configured_or_the_systems(monkeypatch):
    monkeypatch.setenv("TZ", "Asia/Tokyo")
    monkeypatch.setattr(config, "location", LocationSettings())
    assert home_zone().key == "Asia/Tokyo"
    monkeypatch.setattr(config, "location", KRAKOW)
    assert home_zone().key == "Europe/Warsaw"
    # A POSIX rule is not a zone name; the system's zone file is used instead.
    monkeypatch.setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3")
    monkeypatch.setattr(config, "location", LocationSettings())
    assert datetime.now(home_zone()).utcoffset() is not None


def test_overnight_pauses_keep_their_wall_times_across_dst(krakow):
    spring = paused_periods(NIGHT, at("2026-03-28 12:00"), at("2026-03-30 12:00"))
    assert spring == [(at("2026-03-28 23:00"), at("2026-03-29 06:00")), (at("2026-03-29 23:00"), at("2026-03-30 06:00"))]
    assert spring[0][1] - spring[0][0] == timedelta(hours=6)
    autumn = paused_periods(NIGHT, at("2026-10-24 12:00"), at("2026-10-25 12:00"))
    assert autumn[0][1] - autumn[0][0] == timedelta(hours=8)
    # A pause in progress is cut at the start, and the end of the range cuts the last one.
    assert paused_periods(NIGHT, at("2026-11-14 03:00"), at("2026-11-15 00:00")) == [
        (at("2026-11-14 03:00"), at("2026-11-14 06:00")), (at("2026-11-14 23:00"), at("2026-11-15 00:00"))]


def test_sun_pauses_follow_the_days_sunrise_and_sunset(krakow):
    now = at("2026-11-14 12:00")
    periods = paused_periods(DAYLIGHT, now, now + timedelta(days=2))
    _, today_sunset = sun_times(date(2026, 11, 14), KRAKOW.latitude, KRAKOW.longitude)
    tomorrow = sun_times(date(2026, 11, 15), KRAKOW.latitude, KRAKOW.longitude)
    assert periods[:2] == [(now, today_sunset), tomorrow]
    evening = {"from": {"sun": "sunset", "offset": -30}, "to": {"sun": "sunrise", "offset": 30}}
    first = paused_periods(evening, now, now + timedelta(days=1))[0]
    assert first[0] == today_sunset - timedelta(minutes=30)
    assert first[1] == tomorrow[0] + timedelta(minutes=30)


def test_a_pause_that_would_invert_is_skipped_and_one_without_night_merges(monkeypatch):
    helsinki = LocationSettings(latitude=60.17, longitude=24.94, timezone="Europe/Helsinki")
    monkeypatch.setattr(config, "location", helsinki)
    until_ten = {"from": {"sun": "sunset", "offset": 0}, "to": {"time": "22:00"}}
    # The sun sets after 22:00 in June, so there is no pause rather than one until the next evening.
    june = at("2026-06-20 12:00", helsinki)
    assert paused_periods(until_ten, june, june + timedelta(days=3)) == []
    march = at("2026-03-01 12:00", helsinki)
    periods = paused_periods(until_ten, march, march + timedelta(days=3))
    assert len(periods) == 3 and all(end.astimezone(home_zone()).hour == 22 for _, end in periods)
    # Polar night: night pauses join into one, and there is no daylight to pause.
    monkeypatch.setattr(config, "location", LocationSettings(latitude=69.65, longitude=18.96, timezone="Europe/Oslo"))
    start = datetime(2026, 12, 19, tzinfo=UTC)
    night = {"from": {"sun": "sunset", "offset": 0}, "to": {"sun": "sunrise", "offset": 0}}
    assert paused_periods(night, start, start + timedelta(days=3)) == [(start, start + timedelta(days=3))]
    assert paused_periods(DAYLIGHT, start, start + timedelta(days=3)) == []


def test_plans_list_the_pauses_of_the_next_days_in_seconds(krakow):
    payload = plan_payload(NIGHT, at("2026-11-14 12:00"))
    assert json.loads(payload) == {
        "radar": "schedule", "id": plan_id(NIGHT), "for": PLAN_DAYS * 86400,
        "off": [[39600 + 86400 * day, 64800 + 86400 * day] for day in range(PLAN_DAYS)],
    }
    assert len(plan_payload(DAYLIGHT, at("2026-06-21 03:00"))) < 512  # MESH_MSG_DATA_SIZE, including the terminator
    assert json.loads(plan_payload(None, at("2026-11-14 12:00"))) == {"radar": "schedule", "id": 0, "for": 0, "off": []}
    assert 0 < plan_id(NIGHT) < 2**32
    assert plan_id({"to": NIGHT["to"], "from": NIGHT["from"]}) == plan_id(NIGHT)
    assert plan_id(DAYLIGHT) != plan_id(NIGHT)


def test_moving_the_home_changes_the_plan_and_removing_it_drops_sun_pauses(monkeypatch, krakow):
    before = plan_id(DAYLIGHT)
    monkeypatch.setattr(config, "location", LocationSettings(latitude=54.35, longitude=18.65, timezone="Europe/Warsaw"))
    assert plan_id(DAYLIGHT) not in {0, before}
    monkeypatch.setattr(config, "location", LocationSettings())
    assert plan_id(DAYLIGHT) == 0
    assert json.loads(plan_payload(DAYLIGHT, datetime.now(UTC)))["off"] == []
    assert plan_id(NIGHT) != 0


def test_rcm_gets_todays_sun_times_in_the_homes_zone(monkeypatch, krakow):
    today = sun_today(at("2026-11-14 12:00"))
    rise, fall = sun_times(date(2026, 11, 14), KRAKOW.latitude, KRAKOW.longitude)
    assert today == {"zone": "CET", "sunrise": f"{rise.astimezone(home_zone()):%H:%M}",
                     "sunset": f"{fall.astimezone(home_zone()):%H:%M}", "ip_place": None}
    monkeypatch.setattr(config, "location", LocationSettings(timezone="Europe/Warsaw", lookup=False))
    assert sun_today(at("2026-07-01 12:00")) == {"zone": "CEST", "sunrise": None, "sunset": None, "ip_place": None}


@pytest.mark.parametrize("schedule", [
    NIGHT, DAYLIGHT, None,
    {"from": {"time": "00:00"}, "to": {"time": "23:59"}},
    {"from": {"sun": "sunset", "offset": -240}, "to": {"time": "22:30"}},
    {"from": {"time": "22:00"}, "to": {"sun": "sunrise", "offset": 240}},
])
def test_valid_presence_schedules(cm, krakow, schedule):
    cm.presence_sensors = {SENSOR: None}
    validate_command("/rcm/ws/1", {"type": "presence_schedule", "switch_id": SENSOR, "schedule": schedule}, cm)


@pytest.mark.parametrize("command", [
    {"schedule": NIGHT, "switch_id": 222},  # an ordinary switch
    {},
    {"schedule": {"from": {"time": "23:00"}}},
    {"schedule": {**NIGHT, "days": [1, 2]}},
    {"schedule": [NIGHT]},
    {"schedule": {"from": {"time": "24:00"}, "to": {"time": "06:00"}}},
    {"schedule": {"from": {"time": "7:00"}, "to": {"time": "06:00"}}},
    {"schedule": {"from": {"time": "07:60"}, "to": {"time": "06:00"}}},
    {"schedule": {"from": {"time": "０７:００"}, "to": {"time": "06:00"}}},
    {"schedule": {"from": {"time": 700}, "to": {"time": "06:00"}}},
    {"schedule": {"from": {"time": "23:00", "offset": 0}, "to": {"time": "06:00"}}},
    {"schedule": {"from": {"sun": "noon", "offset": 0}, "to": {"time": "06:00"}}},
    {"schedule": {"from": {"sun": "sunset", "offset": 241}, "to": {"time": "06:00"}}},
    {"schedule": {"from": {"sun": "sunset", "offset": "30"}, "to": {"time": "06:00"}}},
    {"schedule": {"from": {"sun": "sunset", "offset": True}, "to": {"time": "06:00"}}},
    {"schedule": {"from": {"sun": "sunset", "offset": 1.5}, "to": {"time": "06:00"}}},
    {"schedule": {"from": {"sun": "sunset"}, "to": {"time": "06:00"}}},
    {"schedule": {"from": {"time": "06:00"}, "to": {"time": "06:00"}}},
    {"schedule": {"from": DAYLIGHT["from"], "to": DAYLIGHT["from"]}},
])
def test_invalid_presence_schedules_are_rejected(cm, krakow, command):
    cm.presence_sensors = {SENSOR: None}
    with pytest.raises((ValueError, TypeError)):
        validate_command("/rcm/ws/1", {"type": "presence_schedule", "switch_id": SENSOR, **command}, cm)


def test_sun_schedules_need_the_homes_coordinates(cm, monkeypatch):
    cm.presence_sensors = {SENSOR: None}
    monkeypatch.setattr(config, "location", LocationSettings())
    validate_command("/rcm/ws/1", {"type": "presence_schedule", "switch_id": SENSOR, "schedule": NIGHT}, cm)
    with pytest.raises(ValueError, match="location"):
        validate_command("/rcm/ws/1", {"type": "presence_schedule", "switch_id": SENSOR, "schedule": DAYLIGHT}, cm)


def test_schedules_are_admin_settings():
    assert command_capability("/rcm/ws/1", {"type": "presence_schedule"}) == "admin"


@pytest.mark.parametrize("fields", [
    {"active": 0, "plan": 0, "planLeft": 0}, {"active": 1, "plan": 2**32 - 1, "planLeft": 14 * 86400},
])
def test_heartbeats_report_the_plan_a_sensor_follows(fields):
    validate_payload("/switch/state/root", json.dumps(heartbeat(**fields)))


@pytest.mark.parametrize("fields", [
    {"active": 2}, {"active": True}, {"plan": -1, "planLeft": 0}, {"plan": 2**32, "planLeft": 0},
    {"plan": "1", "planLeft": 0}, {"plan": 1}, {"plan": 1, "planLeft": 14 * 86400 + 1},
])
def test_invalid_plan_reports_are_rejected(fields):
    with pytest.raises((ValueError, TypeError, KeyError)):
        validate_payload("/switch/state/root", json.dumps(heartbeat(**fields)))


def schedule_sends(client):
    return [json.loads(call.args[1]) for call in client.publish.call_args_list
            if call.args[0] == f"/switch/cmd/{SENSOR}" and '"schedule"' in call.args[1]]


@pytest.fixture
def scheduled_env(broker_env, monkeypatch):
    broker, manager, state, client = broker_env
    now = [0]
    monkeypatch.setattr(broker, "_plan_sends", RateLimiter(clock=lambda: now[0]))
    manager.get_switches.return_value = {SENSOR: ("Hall", 3)}
    manager.get_presence_sensors.return_value = {SENSOR: None}
    manager.get_presence_schedules.return_value = {SENSOR: None}
    return broker, manager, state, client, now


async def test_sensors_without_schedule_support_or_a_schedule_get_no_plan(scheduled_env):
    broker, manager, state, client, _now = scheduled_env
    await broker.handle_root_state(json.dumps(heartbeat()))  # firmware without schedules
    manager.get_presence_schedules.return_value = {SENSOR: NIGHT}
    await broker.handle_root_state(json.dumps(heartbeat()))
    state.update_presence.assert_called_with(SENSOR, presence=1, distance=140, radar=1)
    manager.get_presence_schedules.return_value = {SENSOR: None}
    await broker.handle_root_state(json.dumps(heartbeat(active=1, plan=0, planLeft=0)))
    state.update_presence.assert_called_with(SENSOR, presence=1, distance=140, radar=1, active=1)
    assert schedule_sends(client) == []


async def test_sensors_get_their_plan_after_a_restart_and_once_a_day(scheduled_env, krakow):
    broker, manager, state, client, now = scheduled_env
    manager.get_presence_schedules.return_value = {SENSOR: NIGHT}
    # A restarted sensor has lost its plan.
    await broker.handle_root_state(json.dumps(heartbeat(active=1, plan=0, planLeft=0)))
    sent = schedule_sends(client)
    assert len(sent) == 1 and sent[0]["id"] == plan_id(NIGHT) and len(sent[0]["off"]) == PLAN_DAYS
    # Until the sensor confirms it, the plan is not repeated more than once a minute.
    await broker.handle_root_state(json.dumps(heartbeat(active=1, plan=0, planLeft=0)))
    assert len(schedule_sends(client)) == 1
    now[0] = 60
    await broker.handle_root_state(json.dumps(heartbeat(active=0, plan=plan_id(NIGHT), planLeft=PLAN_REFRESH)))
    state.update_presence.assert_called_with(SENSOR, presence=1, distance=140, radar=1, active=0)
    assert len(schedule_sends(client)) == 1
    await broker.handle_root_state(json.dumps(heartbeat(active=0, plan=plan_id(NIGHT), planLeft=PLAN_REFRESH - 1)))
    assert len(schedule_sends(client)) == 2


async def test_a_removed_or_changed_schedule_replaces_the_sensors_plan(scheduled_env, krakow):
    broker, manager, _state, client, now = scheduled_env
    await broker.handle_root_state(json.dumps(heartbeat(active=1, plan=plan_id(NIGHT), planLeft=PLAN_REFRESH)))
    assert schedule_sends(client) == [{"radar": "schedule", "id": 0, "for": 0, "off": []}]
    now[0] = 60
    manager.get_presence_schedules.return_value = {SENSOR: DAYLIGHT}
    await broker.handle_root_state(json.dumps(heartbeat(active=1, plan=plan_id(NIGHT), planLeft=PLAN_REFRESH)))
    assert schedule_sends(client)[-1]["id"] == plan_id(DAYLIGHT)


def test_rcm_saves_schedules_and_sends_the_plan_to_online_sensors(application, monkeypatch, krakow):
    a = application
    a.cm.presence_sensors = {SENSOR: None}
    state = a.main.state_manager
    monkeypatch.setattr(state, "_online_switches", {})
    with a.http.websocket_connect(f"/rcm/ws/1?token={a.token}") as ws:
        assert ws.receive_json()["type"] == "online_status"
        a.mqtt.drain()
        # Offline: saved and shown, and sent once the sensor reports again.
        ws.send_json({"type": "presence_schedule", "switch_id": SENSOR, "schedule": NIGHT})
        assert ws.receive_json() == {"type": "presence_schedule", "switch_id": SENSOR, "schedule": NIGHT}
        assert a.cm.presence_schedules[SENSOR] == NIGHT
        assert a.mqtt.drain() == []
        state.mark_switch_online(SENSOR, int(time()))
        ws.send_json({"type": "presence_schedule", "switch_id": SENSOR, "schedule": DAYLIGHT})
        assert ws.receive_json()["schedule"] == DAYLIGHT
        [(topic, payload, _retain)] = a.mqtt.drain()
        assert topic == f"/switch/cmd/{SENSOR}" and json.loads(payload)["id"] == plan_id(DAYLIGHT)
        ws.send_json({"type": "presence_schedule", "switch_id": SENSOR, "schedule": None})
        assert ws.receive_json()["schedule"] is None
        assert a.cm.presence_schedules[SENSOR] is None
        assert [json.loads(payload) for _, payload, _ in a.mqtt.drain()] == [
            {"radar": "schedule", "id": 0, "for": 0, "off": []}]
        # An invalid schedule closes the connection without saving anything.
        ws.send_json({"type": "presence_schedule", "switch_id": SENSOR, "schedule": {"from": NIGHT["from"]}})
        with pytest.raises(WebSocketDisconnect) as closed:
            ws.receive_json()
        assert closed.value.code == 1008
    assert a.cm.presence_schedules[SENSOR] is None


def test_operators_cannot_change_schedules(application, monkeypatch):
    a = application
    a.cm.presence_sensors = {SENSOR: None}
    monkeypatch.setitem(config.roles, "test@example.com", {"operator"})
    with a.http.websocket_connect(f"/rcm/ws/1?token={a.token}") as ws:
        assert ws.receive_json()["type"] == "online_status"
        ws.send_json({"type": "presence_schedule", "switch_id": SENSOR, "schedule": NIGHT})
        with pytest.raises(WebSocketDisconnect) as closed:
            ws.receive_json()
        assert closed.value.code == 1008
    assert a.cm.presence_schedules == {}


def test_presence_schedules_endpoint_requires_authentication(application, krakow):
    a = application
    a.cm.presence_sensors = {SENSOR: None, 445: None}
    a.cm.presence_schedules = {SENSOR: NIGHT}
    assert a.http.get("/lights/get_presence_schedules").status_code == 401
    response = a.http.get("/lights/get_presence_schedules", headers={"Authorization": f"Bearer {a.token}"})
    body = response.json()
    assert body["schedules"] == {str(SENSOR): NIGHT, "445": None}
    assert body["zone"] in {"CET", "CEST"}
    assert all(len(body[key]) == 5 for key in ("sunrise", "sunset"))
