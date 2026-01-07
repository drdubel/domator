from time import time

from turbacz.websocket import ws_manager


class StateManager:
    def __init__(self):
        self._states: dict[int, dict[str, int]] = {}
        self._online_relays: dict[int, int] = {}
        self._online_switches: dict[int, int] = {}

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
        for id, timestamp in self._online_relays.items():
            if timestamp + 30 < time():
                self.mark_relay_offline(id)

    async def check_switches_if_online(self):
        for id, timestamp in self._online_switches.items():
            if timestamp + 30 < time():
                self.mark_switch_offline(id)

    def get_state(self, relay_id: int, output_id: str) -> int | None:
        return self._states.get(relay_id, {}).get(output_id)

    def get_all(self) -> dict[int, dict[str, int]]:
        return self._states.copy()


# Singleton instance
state_manager = StateManager()
