/// A snapshot of the heating system, from `/heating/ws` or `/api/temperatures`.
class HeatingReading {
  final DateTime timestamp;
  final double cold;
  final double mixed;
  final double hot;
  final double target;
  final double? integral;
  final double? pidOutput;
  final double? kp;
  final double? ki;
  final double? kd;

  HeatingReading({
    required this.timestamp,
    required this.cold,
    required this.mixed,
    required this.hot,
    required this.target,
    this.integral,
    this.pidOutput,
    this.kp,
    this.ki,
    this.kd,
  });

  factory HeatingReading.fromWsMessage(Map<String, dynamic> msg) {
    return HeatingReading(
      timestamp: DateTime.now(),
      cold: (msg['cold'] as num).toDouble(),
      mixed: (msg['mixed'] as num).toDouble(),
      hot: (msg['hot'] as num).toDouble(),
      target: (msg['target'] as num).toDouble(),
      integral: (msg['integral'] as num?)?.toDouble(),
      pidOutput: (msg['pid_output'] as num?)?.toDouble(),
      kp: (msg['kp'] as num?)?.toDouble(),
      ki: (msg['ki'] as num?)?.toDouble(),
      kd: (msg['kd'] as num?)?.toDouble(),
    );
  }

  factory HeatingReading.fromHistoryEntry(Map<String, dynamic> entry) {
    return HeatingReading(
      timestamp: DateTime.fromMillisecondsSinceEpoch((entry['timestamp'] as num).toInt() * 1000),
      cold: (entry['cold'] as num).toDouble(),
      mixed: (entry['mixed'] as num).toDouble(),
      hot: (entry['hot'] as num).toDouble(),
      target: (entry['target'] as num).toDouble(),
    );
  }
}

/// Setpoints editable from the Heating screen, with the single-character
/// command prefix each one uses in the `/heating/ws` outbound protocol.
/// Confirmed from `static/heating.html`'s `onchange` handlers.
enum HeatingSetpoint {
  target('t'),
  integral('I'),
  kp('p'),
  ki('i'),
  kd('d');

  final String commandPrefix;
  const HeatingSetpoint(this.commandPrefix);
}
