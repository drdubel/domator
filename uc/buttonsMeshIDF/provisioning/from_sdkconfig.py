#!/usr/bin/env python3
"""Migrate credentials out of an sdkconfig file into a provisioning CSV.

These values used to be CONFIG_* options compiled into the firmware. Before
deleting them from sdkconfig, lift them into credentials.csv so the devices can
be provisioned with exactly what they already use -- otherwise the mesh would
have to be reconfigured from scratch.

    ./from_sdkconfig.py ../sdkconfig.esp32-turbacz

Writes credentials.csv next to this script. Review the result, then run
./provision.sh. Remember that the sdkconfig still holds the secrets afterwards:
delete those lines once every device has been provisioned.
"""

import argparse
import csv
import os
import re
import sys
from pathlib import Path

# CONFIG_* name -> NVS key in the "domator" namespace.
MAPPING = [
    ("CONFIG_ROUTER_SSID", "router_ssid", True),
    ("CONFIG_ROUTER_PASSWD", "router_pass", True),
    ("CONFIG_MESH_ID", "mesh_id", True),
    ("CONFIG_MESH_AP_PASSWD", "mesh_ap_pass", True),
    ("CONFIG_MQTT_BROKER_URI", "mqtt_uri", True),
    ("CONFIG_MQTT_USER", "mqtt_user", True),
    ("CONFIG_MQTT_PASSWORD", "mqtt_pass", True),
    ("CONFIG_OTA_URL", "ota_url", False),
    ("CONFIG_OTA_TOKEN", "ota_token", False),
]

LINE = re.compile(r'^(CONFIG_[A-Z0-9_]+)="(.*)"$')


def parse_sdkconfig(path: Path) -> dict[str, str]:
    values = {}
    for line in path.read_text().splitlines():
        m = LINE.match(line.strip())
        if m:
            values[m.group(1)] = m.group(2)
    return values


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("sdkconfig", type=Path, help="sdkconfig file to read")
    ap.add_argument("-o", "--output", type=Path,
                    default=Path(__file__).with_name("credentials.csv"))
    ap.add_argument("-f", "--force", action="store_true",
                    help="overwrite an existing credentials.csv")
    args = ap.parse_args()

    if not args.sdkconfig.is_file():
        print(f"error: {args.sdkconfig} not found", file=sys.stderr)
        return 1

    if args.output.exists() and not args.force:
        print(f"error: {args.output} exists; pass --force to overwrite",
              file=sys.stderr)
        return 1

    values = parse_sdkconfig(args.sdkconfig)

    rows, missing = [], []
    for config_name, nvs_key, required in MAPPING:
        value = values.get(config_name, "")
        if not value:
            if required:
                missing.append(config_name)
            continue
        rows.append((nvs_key, "data", "string", value))

    if missing:
        print("error: required settings absent from "
              f"{args.sdkconfig.name}: {', '.join(missing)}", file=sys.stderr)
        return 1

    # Only the first 6 bytes ever reached the air: the old code did
    # memcpy(mesh_id, CONFIG_MESH_ID, 6). Carry that truncation over verbatim,
    # or newly provisioned devices would form a different mesh than the ones
    # already deployed.
    mesh_id = next((value for key, _, _, value in rows if key == "mesh_id"), "")
    if len(mesh_id) > 6:
        print(f"note: mesh_id {mesh_id!r} is {len(mesh_id)} characters; the "
              f"firmware only ever used the first 6 ({mesh_id[:6]!r}), so that "
              "is what was written.", file=sys.stderr)
        rows = [(k, t, e, mesh_id[:6] if k == "mesh_id" else v)
                for k, t, e, v in rows]
    elif len(mesh_id) < 6:
        print(f"error: mesh_id {mesh_id!r} is shorter than the required 6 "
              "characters", file=sys.stderr)
        return 1

    with open(args.output, "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["key", "type", "encoding", "value"])
        w.writerow(["domator", "namespace", "", ""])
        w.writerows(rows)

    os.chmod(args.output, 0o600)

    print(f"Wrote {args.output} ({len(rows)} keys) with mode 600.")
    for key, _, _, value in rows:
        shown = value if key in ("router_ssid", "mesh_id", "mqtt_uri",
                                 "mqtt_user", "ota_url") else f"<{len(value)} chars>"
        print(f"  {key:13s} {shown}")
    print("\nNext: review it, run ./provision.sh, then delete the secret lines")
    print(f"from {args.sdkconfig.name}.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
