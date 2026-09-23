import math


def merge_temperature_series(water, target):
    """Join by probe labels and timestamps, including missing/sparse series."""
    points = {}
    for series, is_target in [(s, False) for s in water] + [(s, True) for s in target]:
        key = "target" if is_target else series.get("metric", {}).get("probe")
        if key not in {"cold", "mixed", "hot", "target"}:
            continue
        for timestamp, raw in series.get("values", []):
            timestamp = float(timestamp)
            if not math.isfinite(timestamp):
                continue
            point = points.setdefault(
                timestamp,
                {
                    "timestamp": timestamp,
                    "cold": None,
                    "mixed": None,
                    "hot": None,
                    "target": None,
                },
            )
            try:
                value = float(raw)
                point[key] = value if math.isfinite(value) else None
            except (ValueError, TypeError):
                pass
    return [points[t] for t in sorted(points)]
