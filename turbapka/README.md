# turbapka

Android companion app for the [Turbacz](../turbacz) home automation backend — Lights, Blinds, and Heating control, authenticated with the same Google account.

## Getting Started

```
flutter pub get
flutter run
```

On first launch, the app asks for your turbacz server's address and stores
it on-device (see `lib/features/server_setup/server_setup_screen.dart`) — no
backend host is baked into the build. Use "Change server" on the sign-in
screen to update it later.

Release APKs require a production keystore. Copy
[`android/key.properties.example`](android/key.properties.example) to
`android/key.properties`, fill in the values, and restrict it to mode `0600`.
Alternatively set `ANDROID_KEYSTORE_PATH`, `ANDROID_KEY_ALIAS`,
`ANDROID_KEYSTORE_PASSWORD` and `ANDROID_KEY_PASSWORD`. Debug builds do not
require these values. Release builds fail instead of falling back to debug
signing. Follow [Flutter's Android signing instructions](https://docs.flutter.dev/deployment/android#sign-the-app).

For the GitHub release workflow, configure `ANDROID_KEYSTORE_BASE64` (the
base64-encoded keystore), the alias/password secrets above, and
`ANDROID_SIGNING_CERT_SHA256` (the expected signing certificate's SHA-256
fingerprint). The workflow verifies the APK before publishing it. Keep a backup
of the keystore; changing the signing identity prevents upgrading an existing
installation in place. Existing APKs signed with the debug key need a migration
plan, usually uninstall/reinstall with any on-device data backed up first.
The custom-scheme OAuth callback remains tracked by issue #92.
