from aioprometheus.collectors import Gauge

from turbacz.settings import config

water_temp = Gauge(
    "water_temperature",
    "Water temperature in Celsius",
    const_labels=config.monitoring.labels,
)
pid_integral = Gauge(
    "pid_integral", "PID integral accumulator", const_labels=config.monitoring.labels
)
pid_output = Gauge("pid_output", "PID output", const_labels=config.monitoring.labels)
pid_target = Gauge("pid_target", "PID target", const_labels=config.monitoring.labels)
pid_multiplier = Gauge(
    "pid_multiplier", "PID multiplier", const_labels=config.monitoring.labels
)
