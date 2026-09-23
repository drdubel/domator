"""Encode the numeric node metrics without allowing additional lines/fields.

Reference: https://docs.influxdata.com/influxdb/v1/write_protocols/line_protocol_reference/
"""

import math


def tag(value):
    value = str(value)
    if (
        not value
        or len(value) > 256
        or any(ord(c) < 32 or ord(c) == 127 for c in value)
        or value.endswith("\\")
    ):
        raise ValueError("Invalid metric tag")
    return (
        value.replace("\\", "\\\\")
        .replace(" ", "\\ ")
        .replace(",", "\\,")
        .replace("=", "\\=")
    )


def number(value):
    if isinstance(value, bool) or not isinstance(value, (int, float, str)):
        raise ValueError("Invalid metric value")  # noqa: TRY004
    result = float(value)
    if not math.isfinite(result):
        raise ValueError("Non-finite metric value")
    return str(result)


def encode(measurement, tags, fields):
    # Measurements and field keys here are application constants.
    return (
        measurement
        + ","
        + ",".join(f"{tag(k)}={tag(v)}" for k, v in tags.items())
        + " "
        + ",".join(f"{tag(k)}={number(v)}" for k, v in fields.items())
    )


def node_metrics(data, parent_name, labels, ping):
    common = {**labels, "id": data["deviceId"], "name": data["name"]}
    node = encode(
        "node_info",
        common,
        {
            "uptime": data["uptime"],
            "clicks": data["clicks"],
            "free_heap": data["freeHeap"],
            **({"ping_time": ping} if ping is not None else {}),
        },
    )
    mesh = encode(
        "mesh_node",
        {
            **common,
            "parent": data["parentId"],
            "parent_name": parent_name,
            "firmware": data["firmware"],
            "type": data["type"],
        },
        {"rssi": data["rssi"]},
    )
    return node, mesh
