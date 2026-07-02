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

/// A legacy single-motor blind, addressed by a hardcoded id ("r1".."r7").
///
/// Mirrors the webapp's hardcoded list in static/blinds.html — the backend
/// has no discovery API for these names/ids, so they are hardcoded here too.
class LegacyBlind {
  final String id;
  final String name;

  /// Raw backend position, 0-999. Null until the first status update arrives.
  int? position;

  LegacyBlind({required this.id, required this.name, this.position});
}

const legacyBlindDefinitions = <(String id, String name)>[
  ('r1', 'Sypialnia R'),
  ('r2', 'Salon 1'),
  ('r3', 'Salon 2'),
  ('r4', 'Salon 3'),
  ('r5', 'Salon 4'),
  ('r6', 'Sypialnia G'),
  ('r7', 'Sypialnia Zo'),
];
