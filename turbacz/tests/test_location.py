"""The home's location: configured, or the town of the public IP address."""

import asyncio
from datetime import UTC, datetime
from unittest.mock import AsyncMock, MagicMock

import httpx2
import pytest

from turbacz import location
from turbacz.presence import plan_id, sun_today
from turbacz.settings import LocationSettings, config
from turbacz.validation import validate_command

from .test_security import application as application

# Shapes of real answers (ipinfo.io's documented example, GeoJS for 8.8.8.8).
IPINFO = {"ip": "203.0.113.7", "city": "Kraków", "region": "Lesser Poland", "country": "PL",
          "loc": "50.0614,19.9366", "timezone": "Europe/Warsaw"}
GEOJS = {"accuracy": 20, "city": "Kraków", "country": "Poland", "country_code": "PL",
         "latitude": "50.0614", "longitude": "19.9366", "timezone": "Europe/Warsaw"}
GEOJS_COUNTRY_ONLY = {"accuracy": 1000, "country": "United States", "country_code": "US",
                      "latitude": "37.751", "longitude": "-97.822", "timezone": "America/Chicago"}
KRAKOW = {"latitude": 50.06, "longitude": 19.94, "place": "Kraków, PL"}
DAYLIGHT = {"from": {"sun": "sunrise", "offset": 0}, "to": {"sun": "sunset", "offset": 0}}
NIGHT = {"from": {"time": "23:00"}, "to": {"time": "06:00"}}


@pytest.fixture
def lookup_on(monkeypatch):
    monkeypatch.setattr(config, "location", LocationSettings(timezone="Europe/Warsaw"))


def test_answers_give_rounded_coordinates_and_the_town():
    assert location.parse_answer(IPINFO) == KRAKOW
    assert location.parse_answer(GEOJS) == KRAKOW
    assert location.parse_answer({**IPINFO, "country": None})["place"] == "Kraków"


@pytest.mark.parametrize("answer", [
    GEOJS_COUNTRY_ONLY, {**GEOJS, "accuracy": 201}, {**GEOJS, "accuracy": "20"}, {**GEOJS, "accuracy": True},
    {**GEOJS, "accuracy": float("nan")}, {k: v for k, v in GEOJS.items() if k != "accuracy"},
    {**GEOJS, "latitude": 50.0614}, {k: v for k, v in IPINFO.items() if k != "city"},
    {**IPINFO, "loc": "50.0614;19.9366"}, {**IPINFO, "loc": "50.06,19.93,1"}, {**IPINFO, "loc": "1e3,5"},
    {**IPINFO, "loc": "91.0,19.9"}, {**IPINFO, "loc": "50.0,181"}, {**IPINFO, "loc": ["50", "19"]},
    {**IPINFO, "city": "Kra\nków"}, {**IPINFO, "city": "<b>Kraków</b>"}, {**IPINFO, "city": "x" * 65},
    {**IPINFO, "city": ""}, [IPINFO], "50.06,19.94",
])
def test_country_level_or_malformed_answers_are_refused(answer):
    with pytest.raises(ValueError):
        location.parse_answer(answer)


def test_configured_coordinates_come_first_and_lookup_can_be_off(monkeypatch, lookup_on):
    assert location.coordinates() is None and location.ip_place() is None
    monkeypatch.setattr(location, "_ip_location", KRAKOW)
    assert location.coordinates() == (50.06, 19.94)
    assert location.ip_place() == "Kraków, PL"
    monkeypatch.setattr(config, "location", LocationSettings(latitude=54.35, longitude=18.65))
    assert location.coordinates() == (54.35, 18.65)
    assert location.ip_place() is None
    monkeypatch.setattr(config, "location", LocationSettings(lookup=False))
    assert location.coordinates() is None and location.ip_place() is None


def client_answering(*answers):
    """A client whose providers answer in turn: a dict, bytes, a status or an exception."""
    asked = []

    def respond(request):
        answer = answers[len(asked)]
        asked.append(str(request.url))
        if isinstance(answer, Exception):
            raise answer
        if isinstance(answer, int):
            return httpx2.Response(answer)
        if isinstance(answer, bytes):
            return httpx2.Response(200, content=answer)
        return httpx2.Response(200, json=answer)

    return httpx2.AsyncClient(transport=httpx2.MockTransport(respond)), asked


async def test_the_second_provider_answers_when_the_first_fails():
    for first in (503, httpx2.ConnectError("unreachable"), b"<html>", b" " * 20000, GEOJS_COUNTRY_ONLY):
        client, asked = client_answering(first, GEOJS)
        async with client:
            assert await location.lookup(client) == KRAKOW
        assert asked == list(location.PROVIDERS)
    client, asked = client_answering(IPINFO)
    async with client:
        assert await location.lookup(client) == KRAKOW
    assert asked == [location.PROVIDERS[0]]
    client, _ = client_answering(429, GEOJS_COUNTRY_ONLY)
    async with client:
        with pytest.raises(LookupError, match="ipinfo.io.*geojs.io"):
            await location.lookup(client)


async def test_the_home_is_located_at_start_and_kept_while_lookups_fail(application, monkeypatch, lookup_on):
    a = application
    waits = []
    answers = [LookupError("offline"), LookupError("offline"), KRAKOW]

    async def lookup(client):
        answer = answers.pop(0)
        if isinstance(answer, Exception):
            raise answer
        return answer

    async def sleep(seconds):
        waits.append(seconds)

    monkeypatch.setattr(location, "lookup", lookup)
    monkeypatch.setattr(location, "sleep", sleep)
    # A restart without the internet still has the town of an earlier lookup.
    a.cm.ip_location = {"latitude": 50.07, "longitude": 19.95, "place": "Kraków, PL"}
    await location.load_ip_location()
    assert location.coordinates() == (50.07, 19.95)
    await location.keep_located()
    assert waits == [60, 600]
    assert a.cm.ip_location == KRAKOW and location.coordinates() == (50.06, 19.94)
    # The same answer again is not written.
    a.cm.set_ip_location = MagicMock()
    answers.append(KRAKOW)
    await location.keep_located()
    a.cm.set_ip_location.assert_not_called()


async def test_no_lookup_with_configured_coordinates_or_when_turned_off(monkeypatch):
    client = MagicMock(side_effect=AssertionError("no requests"))
    monkeypatch.setattr(location.httpx2, "AsyncClient", client)
    for settings in (LocationSettings(latitude=50.06, longitude=19.94), LocationSettings(lookup=False)):
        monkeypatch.setattr(config, "location", settings)
        await location.keep_located()
    client.assert_not_called()


def test_sun_schedules_follow_the_town_of_the_ip_address(cm, monkeypatch, lookup_on):
    cm.presence_sensors = {444: None}
    command = {"type": "presence_schedule", "switch_id": 444, "schedule": DAYLIGHT}
    with pytest.raises(ValueError, match="location"):
        validate_command("/rcm/ws/1", command, cm)
    night = plan_id(NIGHT)
    monkeypatch.setattr(location, "_ip_location", KRAKOW)
    validate_command("/rcm/ws/1", command, cm)
    daylight = plan_id(DAYLIGHT)
    today = sun_today(datetime(2026, 11, 14, 11, tzinfo=UTC))
    assert today["ip_place"] == "Kraków, PL" and today["sunrise"] < today["sunset"]
    # A new town changes the plans that follow the sun, and only those.
    monkeypatch.setattr(location, "_ip_location", {"latitude": 54.35, "longitude": 18.65, "place": "Gdańsk, PL"})
    assert plan_id(DAYLIGHT) not in {0, daylight}
    assert plan_id(NIGHT) == night


def test_rcm_shows_the_town_of_the_ip_address(application, monkeypatch, lookup_on):
    a = application
    monkeypatch.setattr(location, "_ip_location", KRAKOW)
    response = a.http.get("/lights/get_presence_schedules", headers={"Authorization": f"Bearer {a.token}"})
    assert response.json()["ip_place"] == "Kraków, PL"


async def test_startup_loads_the_town_before_mqtt_and_looks_it_up_again(application, monkeypatch, lookup_on):
    a = application
    seen = []
    a.cm.ip_location = KRAKOW
    found = asyncio.Event()

    async def startup():
        seen.append(location.coordinates())

    async def lookup(client):
        found.set()
        return {"latitude": 50.07, "longitude": 19.95, "place": "Kraków, PL"}

    monkeypatch.setattr(a.main.mqtt, "mqtt_startup", startup)
    monkeypatch.setattr(a.main.mqtt, "mqtt_shutdown", AsyncMock())
    monkeypatch.setattr(location, "lookup", lookup)
    from turbacz import broker

    monkeypatch.setattr(broker.ha_bridge, "_resync_task", None)
    async with a.main.lifespan(a.main.app):
        await asyncio.wait_for(found.wait(), 1)
        for _ in range(10):
            await asyncio.sleep(0)
    assert seen == [(50.06, 19.94)]
    assert a.cm.ip_location == {"latitude": 50.07, "longitude": 19.95, "place": "Kraków, PL"}
