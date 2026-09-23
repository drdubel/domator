import pytest

from turbacz.ha.bridge import HABridge
from turbacz.settings import config

BASE = "domator"
PREFIX = "homeassistant"


class FakeClient:
    """Stands in for the gmqtt client; records publishes instead of sending."""

    def __init__(self):
        self.sent: list[tuple[str, str, bool]] = []

    def publish(self, topic, payload=None, qos=0, retain=False):
        self.sent.append((topic, payload, retain))

    def drain(self) -> list[tuple[str, str, bool]]:
        sent, self.sent = self.sent, []
        return sent

    def topics(self) -> list[str]:
        return [t for t, _, _ in self.sent]


class FakeConnectionManager:
    """In-memory stand-in for ConnectionManager, so tests need no database.

    The default fixture models one 8-output relay in a "Kitchen" section with
    two plain lights, one blind pair, and a switch panel with a toggle button
    and a stateful button.
    """

    def __init__(self):
        self.sections = {1: "Default", 2: "Kitchen"}
        # relay_id -> output_id -> (name, section_id, output_idx, auto_off)
        self.outputs = {
            111: {
                "a": ("Ceiling", 2, 0, 0),
                "b": ("Counter", 2, 1, 0),
                "c": ("Blind power", 2, 2, 0),
                "d": ("Blind dir", 2, 3, 0),
                "e": ("Output 5", 2, 4, 0),
            }
        }
        self.blind_pairs = {111: [("c", "d")]}
        self.blind_pair_names = {(111, "c"): "Kitchen blind"}
        self.relays = {111: ("Ocelot", 8)}
        self.switches = {222: ("Pulsar", 3)}
        self.buttons = {222: {"a": 0, "b": 1}}
        self.applied: dict[str, str] = {}

    # -- registry reads --
    def get_sections(self):
        return dict(self.sections)

    def get_outputs(self):
        return {r: dict(o) for r, o in self.outputs.items()}

    def get_named_outputs(self):
        return {
            r: {oid: meta for oid, meta in o.items() if not meta[0].startswith("Output ")}
            for r, o in self.outputs.items()
        }

    def get_blind_pair_outputs(self):
        return {
            (int(r), oid)
            for r, pairs in self.blind_pairs.items()
            for pair in pairs
            for oid in pair
        }

    def get_relay_blind_pairs_with_names(self):
        result = []
        for relay_id, pairs in self.blind_pairs.items():
            relay_name = self.relays[relay_id][0]
            for power_id, dir_id in pairs:
                result.append(
                    {
                        "relay_id": relay_id,
                        "name": self.blind_pair_names.get((relay_id, power_id)) or relay_name,
                        "relay_name": relay_name,
                        "power_id": power_id,
                        "power_name": self.outputs[relay_id][power_id][0],
                        "direction_id": dir_id,
                        "direction_name": self.outputs[relay_id][dir_id][0],
                    }
                )
        return result

    def get_switches(self):
        return dict(self.switches)

    def get_all_buttons(self):
        return {s: dict(b) for s, b in self.buttons.items()}

    # -- discovery bookkeeping --
    def get_applied_topics(self):
        return dict(self.applied)

    def upsert_applied_topic(self, topic, uid):
        self.applied[topic] = uid

    def delete_applied_topic(self, topic):
        self.applied.pop(topic, None)


@pytest.fixture(autouse=True)
def ha_enabled():
    """The bridge is inert unless enabled; turn it on for every test."""
    previous = config.ha.enabled
    config.ha.enabled = True
    yield
    config.ha.enabled = previous


@pytest.fixture
def cm():
    return FakeConnectionManager()


@pytest.fixture
def client():
    return FakeClient()


@pytest.fixture
def bridge(cm, client):
    b = HABridge()
    b._cm = cm
    b._client = client
    return b
