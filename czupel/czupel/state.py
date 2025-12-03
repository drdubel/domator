from typing import Dict


class RelayStateManager:
    def __init__(self):
        self._states: Dict[str, int] = {}

    def update(self, light_id: str, state: int):
        self._states[light_id] = state

    def get(self, light_id: str) -> int | None:
        return self._states.get(light_id)

    def get_all(self) -> Dict[str, int]:
        return self._states.copy()


# Singleton instance
relay_state = RelayStateManager()
