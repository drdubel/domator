import 'dart:convert';

import 'package:http/http.dart' as http;

import '../config.dart';

/// Thin REST wrapper that attaches the bearer token to every request.
class ApiClient {
  final String token;

  ApiClient(this.token);

  Future<dynamic> getJson(String path, [Map<String, dynamic>? query]) async {
    final response = await http.get(
      AppConfig.httpUri(path, query),
      headers: {'Authorization': 'Bearer $token'},
    );

    if (response.statusCode != 200) {
      throw ApiException(response.statusCode, response.body);
    }

    return jsonDecode(response.body);
  }
}

class ApiException implements Exception {
  final int statusCode;
  final String body;

  ApiException(this.statusCode, this.body);

  @override
  String toString() => 'ApiException($statusCode): $body';
}
