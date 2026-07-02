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
