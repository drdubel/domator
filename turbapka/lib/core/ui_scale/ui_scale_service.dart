import 'package:flutter/foundation.dart';
import 'package:shared_preferences/shared_preferences.dart';

/// Persisted interface-scale preference, applied globally via `main.dart`'s
/// `MaterialApp.builder` (MediaQuery textScaler) plus explicit multiplication
/// at a handful of high-impact layout constants (see theme.dart, lights_screen.dart).
class UiScaleService extends ChangeNotifier {
  static const _prefsKey = 'ui_scale';
  static const double min = 0.8;
  static const double max = 1.5;
  static const double _default = 1.0;

  double _scale = _default;
  double get scale => _scale;

  Future<void> load() async {
    final prefs = await SharedPreferences.getInstance();
    _scale = (prefs.getDouble(_prefsKey) ?? _default).clamp(min, max);
    notifyListeners();
  }

  Future<void> setScale(double value) async {
    _scale = value.clamp(min, max);
    notifyListeners();

    final prefs = await SharedPreferences.getInstance();
    await prefs.setDouble(_prefsKey, _scale);
  }
}
