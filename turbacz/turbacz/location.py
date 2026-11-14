"""The home's location, for sunrise and sunset.

[location] latitude and longitude in turbacz.toml, or else the town of the
public IP address. Turbacz looks it up when it starts and keeps the last
answer in the database, so sun times survive a restart while the internet is
unreachable. A town is precise enough: across one, sunrise and sunset move
by a minute or two.
"""

import json
import logging
import re
from asyncio import sleep
from itertools import chain, repeat

import httpx2

from turbacz.settings import config

logger = logging.getLogger(__name__)

# Free and keyless; the next one is asked when one fails.
PROVIDERS = ("https://ipinfo.io/json", "https://get.geojs.io/v1/ip/geo.json")
RETRY_SECONDS = (60, 600, 3600)  # before each new attempt, then hourly
MAX_ANSWER = 16384
# Where only the country is known, the answer is its middle: sunset could be
# off by an hour.
MAX_ACCURACY_KM = 200
COORDINATE = re.compile(r"-?[0-9]{1,3}(?:\.[0-9]{1,8})?")
PLACE = re.compile(r"[^\x00-\x1f<>]{1,64}")

_ip_location: dict | None = None  # {"latitude", "longitude", "place"} of the IP address


def coordinates() -> tuple[float, float] | None:
    """Latitude and longitude for sun times, or None while unknown."""
    location = config.location
    if location.has_coordinates:
        return location.latitude, location.longitude
    if location.lookup and _ip_location:
        return _ip_location["latitude"], _ip_location["longitude"]
    return None


def ip_place() -> str | None:
    """The town the coordinates were found for, when they come from the IP address."""
    if config.location.has_coordinates or not config.location.lookup or not _ip_location:
        return None
    return _ip_location["place"]


def parse_answer(data) -> dict:
    """{"latitude", "longitude", "place"} from an ipinfo.io or GeoJS answer."""
    if not isinstance(data, dict):
        raise ValueError("Unexpected answer")  # noqa: TRY004
    if "loc" in data:  # ipinfo.io: "50.0614,19.9366"
        latitude, longitude = data["loc"].split(",") if isinstance(data["loc"], str) else (None, None)
    else:  # GeoJS, with how sure it is
        latitude, longitude = data.get("latitude"), data.get("longitude")
        accuracy = data.get("accuracy")
        if type(accuracy) not in (int, float) or not accuracy <= MAX_ACCURACY_KM:
            raise ValueError("Only the country is known")
    if not all(isinstance(value, str) and COORDINATE.fullmatch(value) for value in (latitude, longitude)):
        raise ValueError("Invalid coordinates")
    latitude, longitude = float(latitude), float(longitude)
    if not (-90 < latitude < 90 and -180 <= longitude <= 180):
        raise ValueError("Coordinates out of range")
    city = data.get("city")
    if not isinstance(city, str) or not PLACE.fullmatch(city):
        raise ValueError("Only the country is known")
    country = data.get("country_code", data.get("country"))
    place = f"{city}, {country}" if isinstance(country, str) and re.fullmatch(r"[A-Z]{2}", country) else city
    return {"latitude": round(latitude, 2), "longitude": round(longitude, 2), "place": place}


async def lookup(client: httpx2.AsyncClient) -> dict:
    """Ask the providers in turn where the public IP address is."""
    errors = []
    for url in PROVIDERS:
        try:
            async with client.stream("GET", url, headers={"Accept": "application/json"}, timeout=10) as response:
                response.raise_for_status()
                content = bytearray()
                async for chunk in response.aiter_bytes(4096):
                    content.extend(chunk)
                    if len(content) > MAX_ANSWER:
                        raise ValueError("Answer too large")
            return parse_answer(json.loads(content))
        except (httpx2.HTTPError, ValueError, TypeError, RecursionError) as exc:
            errors.append(f"{url}: {exc!r}")
    raise LookupError("; ".join(errors))


async def load_ip_location():
    """The answer of an earlier lookup, before anything needs sun times."""
    global _ip_location
    # Imported here: they import this module.
    from turbacz.connection_manager import connection_manager
    from turbacz.database import db_call

    _ip_location = await db_call(connection_manager.get_ip_location)


async def keep_located():
    """Look the IP address up again; while that fails, keep the stored answer and retry."""
    global _ip_location
    from turbacz.connection_manager import connection_manager
    from turbacz.database import db_call

    if config.location.has_coordinates or not config.location.lookup:
        return
    async with httpx2.AsyncClient() as client:
        for retry in chain(RETRY_SECONDS, repeat(RETRY_SECONDS[-1])):
            try:
                found = await lookup(client)
            except LookupError as exc:
                logger.warning("Could not locate the home by its IP address: %s", exc)
                await sleep(retry)
                continue
            if found != _ip_location:
                await db_call(connection_manager.set_ip_location, found)
                _ip_location = found
                logger.info("Home located by its IP address: %s", found["place"])
            return
