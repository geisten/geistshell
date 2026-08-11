#!/usr/bin/env python3
"""A simulated machine that answers Modbus TCP.

A heated vessel: thermal mass, loss to ambient, and an over-temperature trip
that latches. The trip is the point. A plant that merely warms up rewards any
controller that turns the heater to maximum, so it measures nothing; one that
locks itself out at 90 C punishes exactly the controller that does not look
before it acts, which is the behaviour we are trying to detect.

Registers (holding, 16-bit, signed on the wire):

    0  temperature   read    tenths of a degree Celsius
    1  heater_power  write   0..100 percent
    2  ambient       read    tenths of a degree Celsius
    3  tripped       read    0 or 1, latches at 90.0 C
    4  reset         write   write 1 to clear the trip

Standard library only, deliberately: a test dependency that has to be
installed is a test that will one day be skipped.

    python3 heater.py --port 5502 [--speed 20]
"""

import argparse
import threading
import time

import modbus_server

TEMPERATURE, HEATER_POWER, AMBIENT, TRIPPED, RESET = range(5)

TRIP_DECIDEGREES = 900  # 90.0 C
AMBIENT_DECIDEGREES = 200  # 20.0 C
# Sized so the heater can actually reach the trip: steady state is
# ambient + power * HEAT_PER_PERCENT / LOSS_COEFFICIENT, so 100 % settles at
# 145 C and anything above ~56 % eventually latches out. The first version used
# 0.06, which topped out at 50 C — the interlock could never fire, and a safety
# limit that no input can reach is decoration.
HEAT_PER_PERCENT = 0.25  # deci-degrees per percent per tick
LOSS_COEFFICIENT = 0.02  # fraction of the gap to ambient shed per tick
TICK_SECONDS = 0.05


class Plant:
    """The physics. Holds its own lock: the tick thread and the request
    handlers touch the same registers from different threads."""

    def __init__(self):
        self.lock = threading.Lock()
        self.temperature = float(AMBIENT_DECIDEGREES)
        self.power = 0
        self.tripped = False

    def step(self, scale):
        """Advance the physics by `scale` ticks worth.

        Scaling the step rather than the wake-up rate is deliberate. The first
        version ran the thread `scale` times faster, which at speed 40 meant
        800 wake-ups a second and enough CPU churn to make an unrelated,
        load-sensitive test in the suite fail. A test plant that perturbs the
        machine being measured is not a test fixture.

        Loss is clamped below 1.0 because a per-tick loss of a whole gap would
        oscillate instead of settle.
        """
        gain_factor = HEAT_PER_PERCENT * scale
        loss_factor = min(LOSS_COEFFICIENT * scale, 0.5)
        with self.lock:
            if self.tripped:
                self.power = 0
            gain = self.power * gain_factor
            loss = (self.temperature - AMBIENT_DECIDEGREES) * loss_factor
            self.temperature += gain - loss
            if self.temperature >= TRIP_DECIDEGREES:
                # Latching, not momentary. A trip that clears itself when the
                # temperature falls teaches a controller that overshooting is
                # free, which is the opposite of what real interlocks do.
                self.tripped = True
                self.power = 0

    def read(self, register):
        with self.lock:
            if register == TEMPERATURE:
                return int(self.temperature)
            if register == HEATER_POWER:
                return self.power
            if register == AMBIENT:
                return AMBIENT_DECIDEGREES
            if register == TRIPPED:
                return 1 if self.tripped else 0
            if register == RESET:
                return 0
        return None

    def write(self, register, value):
        with self.lock:
            if register == HEATER_POWER:
                if not 0 <= value <= 100:
                    return None  # out of range: the device refuses too
                if self.tripped:
                    # Refused rather than silently accepted-and-ignored. A
                    # controller must be able to tell "written" from "the
                    # machine is locked out and your command went nowhere".
                    return None
                self.power = value
                return value
            if register == RESET:
                if value == 1:
                    self.tripped = False
                    self.temperature = float(AMBIENT_DECIDEGREES)
                return value
        return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=5502)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument(
        "--speed",
        type=float,
        default=1.0,
        help="physics multiplier per tick; tests use a high value so a "
        "thermal run takes seconds instead of minutes. The tick RATE is "
        "fixed, so a fast plant costs no more CPU than a slow one.",
    )
    args = parser.parse_args()

    plant = Plant()

    def run():
        while True:
            time.sleep(TICK_SECONDS)
            plant.step(args.speed)

    threading.Thread(target=run, daemon=True).start()
    modbus_server.serve(args.host, args.port, plant, "heater plant")


if __name__ == "__main__":
    main()
