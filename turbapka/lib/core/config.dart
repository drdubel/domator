/// Backend connection settings.
///
/// Set [host] to the domain where turbacz is deployed (no scheme, no path).
class AppConfig {
  static const String host = 'czupel.dry.pl';

  static const String callbackUrlScheme = 'turbacz';

  static Uri httpUri(String path, [Map<String, dynamic>? queryParameters]) {
    return Uri.https(host, path, queryParameters);
  }

  static Uri wsUri(String path, {required String token}) {
    return Uri(
      scheme: 'wss',
      host: host,
      path: path,
      queryParameters: {'token': token},
    );
  }
}
