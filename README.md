# Domator

An opinionated home-automation system: a FastAPI backend, a web app, an
Android app, and microcontroller firmware, talking to each other over MQTT
and WebSockets.

## How it fits together

```
[ turbapka (Android) ]   [ web app (static/) ]
            \                  /
             \                /
           WebSocket / HTTPS (JWT auth)
                    |
              [ turbacz backend ]
              FastAPI + PostgreSQL
                    |
                  MQTT
                    |
        [ uc/ firmware on microcontrollers ]
        blinds, heating, lighting, buttons
```

- **[turbacz](turbacz/)** — the backend: a FastAPI server with a bundled
  static web app, Google OIDC login, PostgreSQL storage, and an MQTT client
  that talks to the microcontrollers. This is the only piece every client
  depends on. See [turbacz/README.md](turbacz/README.md) for setup
  (including a Docker Compose stack with Postgres, Mosquitto, Grafana, and
  VictoriaMetrics).
- **[turbapka](turbapka/)** — the Flutter Android companion app (Lights,
  Blinds, Heating), authenticated against the same Google account as the web
  app. The backend address isn't baked into the build — it's entered on
  first launch and stored on-device, so the same APK works against anyone's
  own turbacz deployment. See [turbapka/README.md](turbapka/README.md).
- **[uc/](uc/)** — microcontroller firmware (PlatformIO/ESP-IDF) for blinds,
  heating, and mesh-networked buttons/relays.
- **[higromaramu](higromaramu/)** — firmware for a standalone
  hygrometer/pressure sensor node.
- **[simulators](simulators/)** — Python scripts that simulate
  microcontrollers over MQTT, for developing the backend without real
  hardware.

## Getting started

Most day-to-day work happens in one of two places:

- Backend/web app changes → follow [turbacz/README.md](turbacz/README.md).
- Android app changes → follow [turbapka/README.md](turbapka/README.md).

Firmware changes are scoped to the relevant subdirectory under
[uc/](uc/) or [higromaramu/](higromaramu/), each with its own
`platformio.ini`.
