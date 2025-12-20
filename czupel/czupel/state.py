from typing import Dict


class RelayStateManager:
    def __init__(self):
        self._states: Dict[str, int] = {}
        self._ping_times: Dict[str, float] = {}
        self._send_ping_time: Dict[str, float] = {}
        self.relays: list[str] = ["1074130365", "1074122133"]

    def route_lights(self, relay_id: str, output_id: int):
        return f"s{(self.relays.index(relay_id) + 1) * 8 + output_id}"

    def deroute_lights(self, light_id: str):
        light_num = int(light_id[1:])
        relay_idx = (light_num // 8) - 1
        output_id = light_num % 8
        return self.relays[relay_idx], output_id

    def update_state(self, light_id: str, state: int):
        self._states[light_id] = state

    def update_ping_time(self, relay_id: str, time: float):
        self._ping_times[relay_id] = time - self._send_ping_time.get(relay_id, 0)

    def update_send_ping_time(self, relay_id: str, time: float):
        self._send_ping_time[relay_id] = time

    def get_state(self, light_id: str) -> int | None:
        return self._states.get(light_id)

    def get_ping_time(self, light_id: str = "", relay_id: str = "") -> float | None:
        if relay_id:
            rid = relay_id
        else:
            rid, _ = self.deroute_lights(light_id)

        return round(self._ping_times.get(rid, 0) / 1_000_000, 2)

    def get_all(self) -> Dict[str, int]:
        return self._states.copy()


# Singleton instance
relay_state = RelayStateManager()
