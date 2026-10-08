import importlib.util
import subprocess
from pathlib import Path

import pytest


@pytest.fixture
def recovery(tmp_path, monkeypatch):
    spec = importlib.util.spec_from_file_location(
        "mqtt_recovery", Path(__file__).resolve().parents[1] / "repair-mqtt.py"
    )
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    monkeypatch.chdir(tmp_path)
    monkeypatch.setattr(module.getpass, "getpass", lambda _: "")
    (tmp_path / "turbacz.toml").write_text(
        '[mqtt]\nusername = "turbacz"\npassword = "existing-password"\n'
    )
    return module


def test_recovery_uses_existing_password_and_removes_empty_directory(recovery, monkeypatch):
    Path("mosquitto.passwd").mkdir()
    config_before = Path("turbacz.toml").read_bytes()

    def hash_file(command, **kwargs):
        folder = command[command.index("--volume") + 1].removesuffix(":/work")
        password_file = Path(folder) / "passwords"
        assert password_file.read_text() == "turbacz:existing-password\n"
        assert password_file.stat().st_mode & 0o777 == 0o600
        password_file.write_text("turbacz:$7$hashed-placeholder\n")

    monkeypatch.setattr(recovery.subprocess, "run", hash_file)
    recovery.repair()
    assert Path("mosquitto.passwd").read_text() == "turbacz:$7$hashed-placeholder\n"
    assert Path("mosquitto.passwd").stat().st_mode & 0o777 == 0o600
    assert Path("turbacz.toml").read_bytes() == config_before
    assert not list(Path.cwd().glob(".mqtt-repair-*"))


def test_hash_failure_cleans_plaintext_and_preserves_directory(recovery, monkeypatch):
    Path("mosquitto.passwd").mkdir()

    def fail(command, **kwargs):
        raise subprocess.CalledProcessError(1, command)

    monkeypatch.setattr(recovery.subprocess, "run", fail)
    with pytest.raises(subprocess.CalledProcessError):
        recovery.repair()
    assert Path("mosquitto.passwd").is_dir()
    assert not list(Path.cwd().glob(".mqtt-repair-*"))


@pytest.mark.parametrize("directory", [False, True])
def test_recovery_preserves_existing_credentials_and_nonempty_directories(recovery, directory):
    if directory:
        Path("mosquitto.passwd").mkdir()
        existing = Path("mosquitto.passwd/backup")
    else:
        existing = Path("mosquitto.passwd")
    existing.write_text("preserve-me")
    with pytest.raises(ValueError):
        recovery.repair()
    assert existing.read_text() == "preserve-me"
