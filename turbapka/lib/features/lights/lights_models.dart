/// A named output (light) belonging to a relay, grouped into a section.
///
/// Mirrors the `named_outputs` payload: `{output_id: [name, section_id, output_idx, auto_off_seconds]}`.
class LightOutput {
  final int relayId;
  final String outputId;
  final String name;
  int sectionId;
  int outputIdx;
  bool isOn;

  LightOutput({
    required this.relayId,
    required this.outputId,
    required this.name,
    required this.sectionId,
    this.outputIdx = 0,
    this.isOn = false,
  });

  String get key => '$relayId$outputId';
}

class LightSection {
  final int id;
  final String name;

  LightSection({required this.id, required this.name});
}
