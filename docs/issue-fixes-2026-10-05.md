GitHub issue review — 2026-10-05
================================

Reviewed all 19 open issues against the checkout and the published default
branch (`cd4090d`). Closed #85 (both OAuth error fields escaped), #87 (the
privileged opencode workflow was removed), and #98 based on the owner's
confirmation that exposed credentials have already been rotated. New changes below are local and
must be reviewed, merged and deployed before their issues are closed.

| Issue | Result in this change | Remaining work |
| --- | --- | --- |
| #90 | Authenticate metrics/history; dedicated scrape bearer token; history rate, interval, point and upstream response bounds. | Configure the scraper token and deploy. |
| #100 | Viewer/operator/admin/OTA assignments, JWT role claims, HTTP mutation checks and per-message WebSocket authorization; role/allowlist changes use current configuration. | Assign existing users, restart and deploy. Admin includes all capabilities. Optional recent step-up authentication remains a follow-up. |
| #101 | Exact frame size, per-type lengths and values, bounded text termination, unknown-type rejection, safe routing snapshots, allocation/mutex checks. Exhaustive host sanitizer test and fuzz target. | Build/flash and validate on ESP hardware. Sender authentication remains #103. |
| #105 | Reject malformed commands, validate IDs/positions, bounded snprintf, accept only S as the all-blinds state query. | Build/flash and verify UART behavior on the ESP8266. |
| #108 | ASGI stream limit independent of Content-Length; existing authentication/rate/concurrency checks precede parsing; bounded file reads, private unique temporary files and atomic replacement. | Apply equivalent reverse-proxy limit and test deployed proxy behavior. Limits are per process. |
| #110 | UTF-8/size checks, per-topic work limit, complete telemetry schemas before state mutation, exception isolation; broker packet/queue bounds. | Deploy broker/backend; publisher-specific quotas still need broker-side enforcement. |
| #112 | Explicit/env/dotenv/nested-secret/TOML precedence, custom config path, private atomic generated files, read-only config mount. Existing local TOML and hygrometer credential file changed to mode 0600 without reading their values. | Configure running secret delivery. Owner confirmed exposed credentials were already rotated. |
| #88 | Random new-install database/Grafana passwords; require Compose secrets; loopback service ports; versioned VM/HA images; build-context exclusions. | Migrate existing deployment secrets, review container UID/capability/resource hardening and image digests. |
| #91 | Re-check allowlist for every JWT verification and re-authenticate live socket messages. | Short-lived access/refresh lifecycle, token/session revocation, config reload and removing query-string JWTs. |
| #92 | Release keystore required; CI checks expected signing certificate; Gradle distribution checksum. | Supply signing identity/secrets and upgrade plan; replace custom-scheme token delivery with a bound code exchange/App Links. |
| #93 | Telnet listener compiled out by default; explicit development opt-in. | Provision broker TLS/CA and migrate all clients, including the mesh root. |
| #104 | HA uses backend command grammar; firmware rejects empty/invalid/non-finite numbers without exceptions and throttles EEPROM writes. | Installation-approved setpoint/PID bounds, valid stored defaults and independent physical safety cutoff. No physical defaults were guessed. |
| #102 | Firmware downloads use private/no-store; role checks cover upload and OTA commands. | Signed image verification, key custody/device provisioning, short-lived device authorization and tested boot rollback. |
| #111 | PR Python/browser/Flutter checks and host sanitizer/fuzz jobs, action SHA pins, fixed Flutter/uv versions, Gradle checksum, lockfile-managed psycopg binary extra, build-context secret exclusions and APK signing verification. | CI execution/required-check settings, CodeQL/dependency/secret/container/SBOM scans, firmware dependency locks, image digests and secret scanning of firmware artifacts. |
| #98 | Closed based on the owner's confirmation of completed credential rotation. No history rewrite performed. | History rewriting is optional and remains unrequested. |
| #99 | No runtime provisioning migration performed for ESP8266 devices. | Design/store/provision per-device runtime credentials and remove old secret-bearing artifacts after rollout. |
| #103 | Structural validation fixed separately from cryptographic authentication. | Per-device keys, authenticated frames, binding logical identity to physical identity and replay/rotation/revocation protocol. |

The owner explicitly deferred heating limits, signing-key setup and TLS rollout.
No physical setpoint/PID policy, signing keys or TLS deployment were configured.

Deployment steps for existing installations
------------------------------------------

1. Back up configuration and persistent data. Do not run `setup.sh` over an
   existing installation; it now refuses to overwrite existing secrets.
2. At the top level of `turbacz.toml`, assign the owner explicitly:
   `roles = { "your-email@example.com" = ["admin"] }`. Add operators/viewers as
   appropriate; the `authorized` allowlist still applies. Unassigned users are
   viewers. Restart after configuration edits; no live file watcher exists.
3. In `[monitoring]`, set a fresh random `scrape_token`; put the identical token
   in `monitoring/scrape_token` and chmod it to 0600. Compose mounts it only
   into VictoriaMetrics. Anonymous `/metrics` requests now return 401.
4. Create a private `.env` with `POSTGRES_PASSWORD` matching the CURRENT
   database password and `GRAFANA_ADMIN_PASSWORD`. Setting a Compose password
   does not change passwords inside existing PostgreSQL/Grafana volumes;
   rotate those accounts separately through their supported administration
   interfaces and update the backend's `[psql].password` at the same time.
5. Set `DOMATOR_MQTT_BIND` in `.env` to the host's exact LAN IP when devices
   connect over LAN. The default is loopback. Use a firewall to restrict
   broker access. Grafana, HA and VM now bind to loopback; access through the
   intended reverse proxy or an SSH tunnel. Verify the pinned VM/HA upgrade
   against backups before recreating their services.
6. Rebuild/restart the backend and broker; confirm scrapes and the one-hour
   heating chart work and operator/admin permissions match the assignments.
   At the reverse proxy enforce a 10,000,000-byte request limit and upload
   timeouts (for nginx: `client_max_body_size 10000000;`). Request multipart
   overhead counts toward this limit. Upload/telemetry quotas remain per process.
7. Build the modified mesh/blinds/heating firmware and check on test devices
   before flashing the installation. Telnet is disabled unless explicitly
   enabled with `CONFIG_DOMATOR_DEBUG_TELNET`; serial logs remain available.
   Parser fixes do not implement the installation's physical safety cutoff.
8. Follow the signing setup in `turbapka/README.md` before releasing an APK.
   Exposed credential rotation is already completed per the owner. Address
   the deferred hardware security issues as separate installation work.

Secret delivery
---------------

Settings precedence is: explicit constructor values, `TURBACZ_` environment
variables, an explicitly supplied dotenv file, nested secret-directory files,
then TOML, then defaults. Nested values use `__`, for example
`TURBACZ_MQTT__PASSWORD`. Set `TURBACZ_CONFIG_PATH` for a custom TOML path;
set `TURBACZ_SECRETS_DIR=/run/secrets` and mount files such as
`/run/secrets/TURBACZ_JWT_SECRET` and
`/run/secrets/TURBACZ_MQTT__PASSWORD` for Docker/Kubernetes secret delivery.
The same prefix/delimiter applies to file names. Existing TOML files are not
rewritten and no existing credential values were read or rotated by the agent. The owner
confirmed rotation was completed independently.
See [Pydantic's settings documentation](https://docs.pydantic.dev/latest/concepts/pydantic_settings/)
and [Starlette's body-limit middleware](https://www.starlette.io/middleware/).

Verification
------------

Passed: 168 Python tests (one skipped), 11 browser tests, Flutter analysis and
11 Flutter tests, Android debug APK build, and host ASan/UBSan tests for mesh,
remote, blinds and heating parsers. Missing production signing material is
rejected by the Gradle release-signing validation task as expected.

Blinds and heating ESP8266 firmware compiled with disposable placeholder
credentials, and ESP32-C3 mesh firmware compiled with Telnet excluded. These
are build checks, not device tests. Do not flash the ESP8266 test binaries:
their credentials are deliberately placeholders. The mesh build still reports
an existing 2 MB/4 MB flash-size configuration mismatch; confirm the target
hardware/profile before flashing. No devices were flashed.

The local Apple Clang installation lacks the libFuzzer runtime. Exhaustive
host sanitizer checks passed; the seeded libFuzzer job is configured for Linux
CI but has not run on GitHub. Workflow/Compose YAML parsed successfully;
Docker Compose is unavailable locally, so service recreation/config expansion
has not been tested. Python fatal/import lint, new-module Ruff checks, shell
syntax and `git diff --check` passed. CI and production deployment remain
outstanding. No commits were pushed.
