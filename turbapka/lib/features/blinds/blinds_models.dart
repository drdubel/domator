/// A motorized blind backed by a relay power/direction output pair.
///
/// Mirrors the `relay_blinds` payload from `/blinds/ws`.
class BlindPair {
  final int relayId;
  final String name;
  final String relayName;
  final String powerId;
  final String powerName;
  final String directionId;
  final String directionName;

  int? powerState;
  int? directionState;

  BlindPair({
    required this.relayId,
    required this.name,
    required this.relayName,
    required this.powerId,
    required this.powerName,
    required this.directionId,
    required this.directionName,
  });

  /// Mirrors `blinds.js`'s `applyCardState`: stopped when power is off,
  /// otherwise up/down depending on direction.
  BlindMotionState get motionState {
    if (powerState == null || directionState == null) return BlindMotionState.unknown;
    if (powerState == 0) return BlindMotionState.stopped;
    return directionState == 0 ? BlindMotionState.movingUp : BlindMotionState.movingDown;
  }
}

enum BlindMotionState { unknown, stopped, movingUp, movingDown }
