class RelayStateManager:
    def __init__(self):
        self._states: dict[int, dict[str, int]] = {}
        self._ping_times: dict[str, float] = {}
        self._send_ping_time: dict[str, float] = {}
        self.relays: list[str] = []

    def update_state(self, relay_id: int, output_id: str, state: int):
        if relay_id not in self._states:
            self._states[relay_id] = {}
        self._states[relay_id][output_id] = state

    def get_state(self, relay_id: int, output_id: str) -> int | None:
        return self._states.get(relay_id, {}).get(output_id)

    def get_all(self) -> dict[int, dict[str, int]]:
        return self._states.copy()


# Singleton instance
relay_state = RelayStateManager()
