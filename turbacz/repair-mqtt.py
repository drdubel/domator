"""Recover a missing broker password file without replacing installation secrets.

Run with Python 3.11+ from the turbacz directory while Mosquitto is stopped.
"""

import getpass
import os
import subprocess
import tempfile
import tomllib
from pathlib import Path


def credential(username: str, password: str) -> str:
    if not username or any(char in username for char in ":\r\n\0"):
        raise ValueError("MQTT username is empty or contains an invalid character")
    if not password or any(char in password for char in "\r\n\0"):
        raise ValueError("MQTT password is empty or contains an invalid character")
    return f"{username}:{password}\n"


def repair() -> None:
    target = Path("mosquitto.passwd")
    if target.is_symlink() or (target.exists() and not target.is_dir()):
        raise ValueError("mosquitto.passwd already exists; restore/edit that file instead")
    if target.is_dir() and any(target.iterdir()):
        raise ValueError("mosquitto.passwd is a nonempty directory; refusing to remove it")

    with Path("turbacz.toml").open("rb") as config_file:
        config = tomllib.load(config_file)
    mqtt = config["mqtt"]
    entries = [credential(mqtt.get("username", ""), mqtt.get("password", ""))]
    print("Using the existing Turbacz MQTT credentials from turbacz.toml.")
    print("Device passwords must match those already compiled into your firmware.")
    for username, label in (
        ("mesh_root", "Mesh root (CONFIG_MQTT_PASSWORD)"),
        ("heating-wifi", "Heating controller"),
        ("blinds-wifi", "Legacy blinds controller"),
        ("homeassistant", "Home Assistant"),
    ):
        password = getpass.getpass(f"{label} password (blank to skip): ")
        if password:
            if username == mqtt["username"]:
                raise ValueError(f"{username} duplicates the configured backend username")
            entries.append(credential(username, password))

    # Hash a private temporary file before touching the target. Mount its parent
    # because mosquitto_passwd replaces the file during hashing.
    with tempfile.TemporaryDirectory(prefix=".mqtt-repair-", dir=".") as temporary:
        folder = Path(temporary).resolve()
        plaintext = folder / "passwords"
        fd = os.open(plaintext, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, "w") as output:
            output.writelines(entries)
        subprocess.run(
            [
                "docker", "run", "--rm", "--network", "none",
                "--user", f"{os.getuid()}:{os.getgid()}",
                "--volume", f"{folder}:/work", "--workdir", "/work",
                "eclipse-mosquitto:2.0", "mosquitto_passwd", "-U", "passwords",
            ],
            check=True,
        )
        plaintext.chmod(0o600)
        if target.is_dir():
            target.rmdir()  # Only an empty directory can be removed.
        # Exclusive creation also protects a file created concurrently.
        os.link(plaintext, target)
    print("Created mosquitto.passwd (0600). Installation configuration is unchanged.")
    print("Run: docker compose up -d mosquitto turbacz homeassistant")


if __name__ == "__main__":
    try:
        repair()
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        raise SystemExit(f"MQTT recovery failed: {error}") from None
