Application fixes — 2026-09-24
=============================

Implemented the nine application issues selected in [the assessment](issue-triage.md),
on top of the merged PR #113 (`f94e41b`). This describes local implementation and
tests; deployment and GitHub issue closure are separate steps.

| Issue | Change |
| --- | --- |
| #95 | HTTP exceptions retain their status and headers; unauthorized light/configuration routes return 401. Validation failures return error statuses. |
| #86 | Stored blind, relay, switch and output names use DOM text/value properties. Blind-pair controls use event listeners. Existing malformed pair IDs are ignored by the browser; form and command paths validate IDs and output membership. |
| #94 | Validate complete WebSocket commands before database writes/MQTT publication. Restrict device types, IDs, outputs and values; encode every metrics tag and numeric field, rejecting control characters and non-finite/injected values. |
| #96 | Pin Sentry's browser bundle with SHA-384 integrity and anonymous CORS. Disable default PII in both SDKs; default trace sampling to 0.1 and expose the configured rate to the browser. An absent DSN loads no browser SDK. |
| #97 | Bound auth attempts by IP, uploads by IP/user and concurrent uploads, and WebSocket connections/messages. Setup defaults to HTTPS with explicit localhost HTTP opt-in. Replace python-jose and its unused crypto dependencies with PyJWT, preserving HS256 tokens with the existing claims. |
| #106 | Always release WebSockets on disconnect/error. Bound transport frames, decoded message collections/depth, connections, message rate, receive idle time and send time. Broadcast concurrently with bounded sends. Run database calls in workers and serialize the shared connection through commit/rollback. |
| #89 | Set both login and OAuth-session cookies Secure by default, independent of proxy-reported scheme. Add CSP, HSTS in HTTPS mode, frame denial, nosniff and a same-origin referrer policy. |
| #107 | Check exact configured Host/Origin values; ignore forwarded host/origin claims. Cookie-authenticated mutations require a session-bound CSRF token. Browser requests obtain it from `/csrf-token`; logout is now POST. Native bearer and explicit WebSocket-token authentication remain supported. |
| #11 | Merge historical and live temperatures into four-second timestamp buckets with an actual one-hour retention bound. Preserve live readings when history arrives later, keep gaps, use the current origin/port, and join backend metric series by probe and timestamp instead of array position. |

**Deployment settings**

Set `oidc.redirect_uri` to the actual HTTPS callback URL, for example
`https://domator.example/auth`. Its exact origin and hostname are also trusted.
Additional public aliases belong in `security.allowed_hosts` and
`security.allowed_origins` in `turbacz.toml`; include ports in origins. The
defaults also permit the internal `turbacz` host used by the metrics scraper.
There are no wildcard host/origin entries. Proxy headers do not add trusted
hosts or origins. If the server needs forwarded client IP/scheme information,
set `server.forwarded_allow_ips` to the actual proxy address and restrict direct
access to the application port.

Local HTTP development requires both `security.allow_insecure_http = true`
and `oidc.allow_insecure_http = true`, with the local HTTP callback/origin
configured explicitly. This disables Secure cookies and HSTS for that instance.
The setup script asks for this opt-in and restricts its HTTP origin to localhost.
Existing TOML files are not rewritten automatically.

Run via the `turbacz` entry point to apply the transport frame/queue limits.
Custom Uvicorn launch commands must use the `websockets` protocol with
`--ws-max-size 65536 --ws-max-queue 16 --ws-per-message-deflate false` (or the
matching configured size). Application limits still run, but checking a message
after transport assembly cannot by itself bound frame buffering.

Rate limits are in-memory and per process, matching the existing single-worker
entry point. They reset on restart; multiple workers/replicas would require a
shared limiter. Defaults and tuning fields are in
`turbacz/turbacz.toml.example`. Browser sockets send a heartbeat; existing native
clients can reconnect after the idle timeout. Do not treat the connection/message
limits as a replacement for the separate upload-stream/proxy work in #108.

CSP deliberately permits the existing inline UI event handlers and styles.
It restricts script origins, objects, forms, base URLs and framing; it does not
provide a strict nonce-based script policy. XSS prevention here relies on safe
DOM assignments and input validation, with regression coverage for both.

**Verification**

Completed: 76 Python tests and 5 browser tests passed. New security modules pass
the configured Ruff rules; changed Python files pass import/error lint. Browser
script syntax, setup shell syntax and `git diff --check` also pass. The broader
repository lint run still reports existing style/type-modernization findings
outside those checks.

From `turbacz/`, run `uv sync --frozen`, `uv run --frozen pytest`, then
`npm ci --ignore-scripts` and `npm test` (Node.js 20.19+).
The Python tests exercise actual HTTP/WebSocket routes with fake database/MQTT
adapters, including rejected requests that must publish no commands, connection
cleanup/limits, CSRF/origin checks, JWT compatibility, metric encoding and
serialized database work. The browser tests use jsdom to parse malicious stored
names, verify token attachment and Sentry options, and check chart time series.
They do not connect to the running house, Google, Sentry or physical devices.

Implementation references: [PyJWT decode API](https://pyjwt.readthedocs.io/en/latest/api.html)
and [InfluxDB line protocol](https://docs.influxdata.com/influxdb/v1/write_protocols/line_protocol_reference/).
