# domator on virgo: turbacz and czupel in jails

One Ansible role, `domator_jail`, builds a VNET jail per instance on virgo
(`virgo.dom.dry.pl`). `turbacz` runs branch `main`; `czupel`, the test
instance, runs branch `czupel`. Both come from the same code and differ only
in their entry in `host_vars/virgo/main.yml`.

| | turbacz | czupel |
|---|---|---|
| branch | `main` | `czupel` |
| jail, network | `turbacz`, epair51, 192.168.51.11 | `czupel`, epair52, 192.168.52.11 |
| database, role | `turbacz` | `czupel` |
| public | https://turbacz.dry.pl | https://czupel.dry.pl |

The playbook uses only the extension points of virgo's collection
(`~/git/dom/virgo`, README "Integracja z innymi projektami"): jail roots are
`dom.virgo.jailbase` clones of the host's own world, the database and the
vhost come from `dom.virgo.postgresql` and `dom.virgo.nginx`, the scrape job
from `dom.virgo.victoriametrics`. virgo's host_vars hold what belongs to the
router: the pf anchor hooks (`pf_anchors`), PostgreSQL's listen addresses and
pg_hba lines, and the doas rules for `domator-jail`.

## Inside a jail

- The jail's world is virgo's (`/usr/local/jails/<name>`); packages come
  from FreeBSD-ports (`pkg -j <name>`).
- The application runs as user `<name>` (`turbacz` keeps uid 1432) under
  `service domator`, from `/home/<name>/current` → `releases/<sha>`.
  `/usr/local/etc/domator/turbacz.toml` is written by Ansible; OTA images
  live in `/var/db/domator/firmware`; logs in `/var/log/domator/`.
- PostgreSQL stays on virgo. The jail connects over TCP to its gateway
  (192.168.5x.10, a permanent epair created at boot) with scram-sha-256.
  A shared Unix socket was rejected: peer authentication cannot tell the
  jail's root from the host's, and virgo maps root to `postgres`.
- MQTT: virgo's broker at 192.168.3.10:1883. Metrics history: virgo's
  VictoriaMetrics on the gateway. Nothing else at home is reachable (pf
  anchor `<name>`); nginx and VictoriaMetrics on virgo reach ports 8000/9000.

## Access for dry and antek

```sh
doas domator-jail czupel                 # shell in the jail as yourself
doas domator-jail turbacz doas service domator restart
```

Both are in `wheel` inside every jail with `permit nopass :wheel`, so they are
root there. On virgo doas allows them the `domator-jail` wrapper only, which
refuses other jails. In PostgreSQL both are members of the `turbacz` and
`czupel` roles (`psql turbacz` on virgo, peer authentication).

## Deployment

1. A push to `main` or `czupel` runs `.github/workflows/turbacz.yml`: the
   turbacz tests on GitHub's runners.
2. If they pass, the `deploy` job calls `POST https://<domain>/_deploy` with a
   bearer token and the commit, then polls `GET /_deploy/status`.
3. nginx on virgo passes `/_deploy` to the hook in the jail
   (`domator-deploy serve`, port 9000). The jail fetches the branch from
   GitHub itself, builds `releases/<sha>` (`git archive` + `uv sync --frozen`),
   switches `current`, restarts `domator` and checks `/login`. If the check
   fails it switches back to the previous release and restarts again
   (`rolled_back`; the GitHub job fails).

Nothing on GitHub can log in to virgo: the token only queues "deploy the tip
of your branch", and the jail decides what that is. By hand, inside the jail:
`/usr/local/libexec/domator-deploy deploy` (as the service user) and
`cat /home/<name>/state/status.json`.

GitHub setup (repository Settings → Environments), per instance:

| environment | secret `DEPLOY_TOKEN` | variable `DEPLOY_URL` |
|---|---|---|
| `turbacz` | `secret_domator.turbacz.deploy_token` | `https://turbacz.dry.pl/_deploy` |
| `czupel` | `secret_domator.czupel.deploy_token` | `https://czupel.dry.pl/_deploy` |

Restrict each environment to its branch (Deployment branches: `main`, `czupel`).

## Secrets

`host_vars/virgo/secrets.sops.yml` (sops, the age key shared with virgo):

```yaml
secret_domator:
  turbacz:
    authorized: [someone@example.com]   # who may log in
    jwt_secret: ...
    session_secret: ...
    mqtt_password: ...                  # mqtt_username defaults to the name
    oidc_client_id: ...
    oidc_client_secret: ...
    psql_password: ...                  # the role's password in PostgreSQL
    firmware_token: ...                 # = CONFIG_OTA_TOKEN in the firmware
    deploy_token: ...                   # openssl rand -hex 32
    # sentry_dsn: ...                   # optional
  czupel:
    ...
```

`mise run secrets` edits it. The values for turbacz are those of today's
`/home/turbacz/git/domator/turbacz/turbacz.toml`; the Google OAuth client
must also list `https://czupel.dry.pl/auth` as a redirect URI.

## First installation

Order matters: virgo's pf.conf loads the anchor files and PostgreSQL binds
the epair addresses, so both must exist first.

```sh
cd ~/git/domator/ansible
mise run check
# 1. epairs, jail roots, jail.conf, pf anchor files (the hook check is off once)
mise run deploy -- --tags host,pf -e domator_pf_require_hook=false
# 2. virgo: pf_anchors, postgresql_listen/hba_extra, doas rules
(cd ~/git/dom/virgo && mise run deploy-step)
ssh virgo doas pfctl -nf /etc/pf.conf && ssh virgo doas pfctl -f /etc/pf.conf
# 3. everything: packages, accounts, config, czupel's first deployment
mise run deploy-step
```

## Cutover of turbacz

`turbacz` has `service: false` while turbacz still runs on the host: the jail
is ready, but nothing in it starts and nginx still points at the host. Only
one turbacz may talk to the broker at a time.

1. Copy the OTA images:
   `doas cp -Rp /home/turbacz/git/domator/turbacz/firmware/. /usr/local/jails/turbacz/var/db/domator/firmware/`.
2. Stop the host instance: `doas service turbacz stop && doas sysrc turbacz_enable=NO`.
3. Set `service: true` for turbacz and run `mise run deploy-step`: first
   deployment, vhost to the jail, scrape target.
4. Check https://turbacz.dry.pl, then remove `/usr/local/etc/rc.d/turbacz`.

Back: `service: false`, deploy, `doas service turbacz start` on the host and
restore the old vhost from `~/git/dom/virgo/inventory/files`.

## Tests

`mise run check`: syntax (also of the role's task files), templates of both
instances rendered with placeholder secrets (`tests/test_role.py`), and the
deployer end to end against a local git origin (`tests/test_deploy.py`).
