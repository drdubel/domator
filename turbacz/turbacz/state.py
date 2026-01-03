from turbacz.websocket import ws_manager


class RelayStateManager:
    def __init__(self):
        self._states: dict[int, dict[str, int]] = {}
        self._ping_times: dict[str, float] = {}
        self._send_ping_time: dict[str, float] = {}
        self.relays: list[str] = []

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

    def get_state(self, relay_id: int, output_id: str) -> int | None:
        return self._states.get(relay_id, {}).get(output_id)

    def get_all(self) -> dict[int, dict[str, int]]:
        return self._states.copy()


# Singleton instance
relay_state = RelayStateManager()
