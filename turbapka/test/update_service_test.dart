import 'package:flutter_test/flutter_test.dart';
import 'package:turbapka/features/updates/update_service.dart';

Map<String, dynamic> _release(
  String tag, {
  bool draft = false,
  bool prerelease = false,
  bool withApk = true,
}) => {
  'tag_name': tag,
  'draft': draft,
  'prerelease': prerelease,
  'body': 'Notes for $tag',
  'assets': [
    if (withApk)
      {
        'name': 'app-release.apk',
        'size': 1234,
        'browser_download_url':
            'https://github.com/drdubel/domator/releases/download/$tag/app-release.apk',
      },
  ],
};

void main() {
  test('compares dotted versions numerically', () {
    expect(UpdateService.compareVersions('1.0.10', '1.0.9'), greaterThan(0));
    expect(UpdateService.compareVersions('1.0', '1.0.0'), 0);
    expect(UpdateService.compareVersions('1.0.2', '1.1.0'), lessThan(0));
  });

  test('offers the newest published turbapka release with an APK', () {
    final update = UpdateService.newestRelease([
      _release('turbapka-v1.0.3'),
      _release('turbapka-v1.0.9', draft: true),
      _release('turbapka-v1.0.8', prerelease: true),
      _release('turbapka-v1.0.7', withApk: false),
      _release('firmware-v9.0.0'),
      _release('turbapka-v1.0.5'),
    ], '1.0.2')!;

    expect(update.version, '1.0.5');
    expect(update.apkSize, 1234);
    expect(update.notes, 'Notes for turbapka-v1.0.5');
    expect(update.apkUrl.path, endsWith('/turbapka-v1.0.5/app-release.apk'));
  });

  test('offers nothing when the app is up to date', () {
    expect(
      UpdateService.newestRelease([_release('turbapka-v1.0.2')], '1.0.2'),
      isNull,
    );
  });
}
