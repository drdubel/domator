import '../../core/network/api_client.dart';
import 'heating_models.dart';

/// Fetches historical readings from `GET /api/temperatures` to backfill the
/// chart, mirroring `heating.js`'s initial one-hour fetch on page load.
class HeatingHistoryApi {
  final ApiClient _client;

  HeatingHistoryApi(this._client);

  Future<List<HeatingReading>> fetchLastHour() async {
    final now = DateTime.now().toUtc();
    final oneHourAgo = now.subtract(const Duration(hours: 1));

    final json = await _client.getJson('/api/temperatures', {
      'start': (oneHourAgo.millisecondsSinceEpoch ~/ 1000).toString(),
      'end': (now.millisecondsSinceEpoch ~/ 1000).toString(),
      'step': '1',
    });

    return (json as List).cast<Map<String, dynamic>>().map(HeatingReading.fromHistoryEntry).toList();
  }
}
