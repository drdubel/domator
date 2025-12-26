from aioprometheus.collectors import Gauge

water_temp = Gauge("water_temperature", "Water temperature in Celsius")
pid_integral = Gauge("pid_integral", "PID integral accumulator")
pid_output = Gauge("pid_output", "PID output")
pid_target = Gauge("pid_target", "PID target")
pid_multiplier = Gauge("pid_multiplier", "PID multiplier")
