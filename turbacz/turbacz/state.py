import hashlib
from time import time

from turbacz.websocket import ws_manager


class StateManager:
    def __init__(self):
        self._states: dict[int, dict[str, int]] = {}
        self._online_relays: dict[int, int] = {}
        self._online_switches: dict[int, int] = {}
        self._firmware_versions: dict[int, str] = {}
        self._up_to_date_devices: dict[int, bool] = {}
        self._up_to_date_firmware_versions: dict[str, str] = {}

        self.update_up_to_date_firmware_versions()

    async def update_state(self, relay_id: int, output_id: str, state: int):
        if relay_id not in self._states:
            self._states[relay_id] = {}

        self._states[relay_id][output_id] = state
        await ws_manager.broadcast(
            {
                "type": "light_state",
                "relay_id": relay_id,
                "output_id": output_id,
                "state": state,
            },
            "/rcm/ws/",
        )
        await ws_manager.broadcast(
            {
                "type": "light_state",
                "relay_id": relay_id,
                "output_id": output_id,
                "state": state,
            },
            "/lights/ws/",
        )

    def set_up_to_date_firmware_version(self, device_type: str, version: str):
        self._up_to_date_firmware_versions[device_type] = version

    def update_up_to_date_firmware_versions(self):
        for device_type in ["relay", "switch", "root"]:
            with open(f"static/data/{device_type}/firmware.bin", "rb") as f:
                version = hashlib.md5(f.read()).hexdigest()
                self._up_to_date_firmware_versions[device_type] = version

    def set_firmware_version(self, device_id: int, device_type: str, version: str):
        self._firmware_versions[device_id] = version

        print(self._up_to_date_firmware_versions, version, device_type, device_id)
        if version == self._up_to_date_firmware_versions.get(device_type):
            self._up_to_date_devices[device_id] = True
        else:
            self._up_to_date_devices[device_id] = False

    def mark_switch_offline(self, switch_id: str):
        if switch_id in self._online_switches:
            del self._online_switches[switch_id]

    def mark_relay_offline(self, relay_id: int):
        if relay_id in self._online_relays:
            del self._online_relays[relay_id]

    def mark_switch_online(self, switch_id: int, timestamp: int):
        self._online_switches[switch_id] = timestamp

    def mark_relay_online(self, relay_id: int, timestamp: int):
        self._online_relays[relay_id] = timestamp

    async def check_relays_if_online(self):
        to_remove = []

        for id, timestamp in self._online_relays.items():
            if timestamp + 30 < time():
                to_remove.append(id)

        for id in to_remove:
            self.mark_relay_offline(id)

    async def check_switches_if_online(self):
        to_remove = []

        for id, timestamp in self._online_switches.items():
            if timestamp + 30 < time():
                to_remove.append(id)

        for id in to_remove:
            self.mark_switch_offline(id)

    def get_state(self, relay_id: int, output_id: str) -> int | None:
        return self._states.get(relay_id, {}).get(output_id)

    def get_all(self) -> dict[int, dict[str, int]]:
        return self._states.copy()


# Singleton instance
state_manager = StateManager()
