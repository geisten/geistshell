"""A simulated machine behind the exec channel.

A heated vessel: thermal mass, loss to ambient, and an over-temperature trip
that latches. The trip is the point. A plant that merely warms up rewards any
controller that turns the heater to maximum, so it measures nothing; one that
locks itself out at 90 C punishes exactly the controller that does not look
before it acts, which is the behaviour we are trying to detect.

The contract is geistshell's exec channel, one invocation per transaction:

    heater.py STATE init [SPEED]      create the vessel (SPEED defaults to 1)
    heater.py STATE CHANNEL           read: prints one integer, exit 0
    heater.py STATE CHANNEL VALUE     write: exit 0 = accepted, echoes VALUE

Channels (values in register units, tenths of a degree Celsius):

    temp      read    vessel temperature
    heater    r/w     0..100 percent
    ambient   read    fixed 200 (20.0 C)
    tripped   read    0 or 1, latches at 90.0 C
    reset     write   write 1 to clear the trip

Physics advance with wall time between invocations, at the same constants the
Modbus ancestor used (a 20 Hz tick, scaled by SPEED), so a test written
against that vessel keeps its timing. Standard library only, deliberately: a
test dependency that has to be installed is a test that will one day be
skipped.

Every invocation is a read-modify-write of one state file, and geistshell
samples all channels of a device CONCURRENTLY (#121). So two of these
processes routinely run at once against the same file, and the state is
handled the way any real channel program sharing state would have to:

  - The whole read-advance-write runs under an exclusive lock on a sidecar
    file. Without it, two readers each advance and save, and the slower one
    silently discards the faster one's update.
  - The file is replaced atomically, never rewritten in place. open(path, "w")
    truncates first and writes second; a concurrent reader in between loads
    an empty file, and a process the executor kills at its deadline between
    the two leaves the plant permanently unreadable.

The lock is a sidecar rather than the state file itself because the state
file is replaced: a lock on the old inode would protect nothing.
"""

import fcntl
import json
import os
import sys
import tempfile
import time

TRIP_DECIDEGREES = 900  # 90.0 C
AMBIENT_DECIDEGREES = 200  # 20.0 C
HEAT_PER_PERCENT = 0.25  # deci-degrees per percent per tick
LOSS_COEFFICIENT = 0.02  # fraction of the gap to ambient shed per tick
TICK_SECONDS = 0.05
MAX_TICKS = 4000  # 200 s of plant time; beyond that everything has settled


def advance(state, now):
    ticks = int((now - state["last"]) / TICK_SECONDS)
    scale = state["speed"]
    gain_factor = HEAT_PER_PERCENT * scale
    loss_factor = min(LOSS_COEFFICIENT * scale, 0.5)
    for _ in range(min(ticks, MAX_TICKS)):
        if state["tripped"]:
            state["power"] = 0
        gain = state["power"] * gain_factor
        loss = (state["temperature"] - AMBIENT_DECIDEGREES) * loss_factor
        state["temperature"] += gain - loss
        if state["temperature"] >= TRIP_DECIDEGREES:
            state["tripped"] = True
            state["power"] = 0
    if ticks > 0:
        state["last"] += ticks * TICK_SECONDS


def save(path, state):
    """Replace the state file atomically: readers see the old state or the
    new one, never a truncated one."""
    directory = os.path.dirname(os.path.abspath(path))
    fd, tmp = tempfile.mkstemp(dir=directory, prefix=".state-")
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            json.dump(state, f)
        os.replace(tmp, path)
    except BaseException:
        os.unlink(tmp)
        raise


def main():
    if len(sys.argv) < 3:
        print(__doc__, file=sys.stderr)
        return 2
    path, channel = sys.argv[1], sys.argv[2]

    if channel == "init":
        speed = int(sys.argv[3]) if len(sys.argv) > 3 else 1
        state = {
            "temperature": float(AMBIENT_DECIDEGREES),
            "power": 0,
            "tripped": False,
            "speed": speed,
            "last": time.time(),
        }
        save(path, state)
        return 0

    # Held until the process exits: released by the kernel even when the
    # executor kills this process at its deadline, so a straggler cannot
    # wedge the plant for the next reader.
    lock = open(path + ".lock", "a", encoding="utf-8")
    fcntl.flock(lock, fcntl.LOCK_EX)

    with open(path, encoding="utf-8") as f:
        state = json.load(f)
    advance(state, time.time())

    value = sys.argv[3] if len(sys.argv) > 3 else None
    if value is None:  # read
        if channel == "temp":
            print(int(state["temperature"]))
        elif channel == "heater":
            print(state["power"])
        elif channel == "ambient":
            print(AMBIENT_DECIDEGREES)
        elif channel == "tripped":
            print(1 if state["tripped"] else 0)
        else:
            return 1
    else:  # write
        value = int(value)
        if channel == "heater":
            # The machine's own interlock: while latched, commands are
            # refused, and geistshell reports that refusal rather than
            # treating an unacknowledged command as done.
            if state["tripped"] or not 0 <= value <= 100:
                return 1
            state["power"] = value
            print(value)
        elif channel == "reset":
            if value == 1:
                state["tripped"] = False
            print(value)
        else:
            return 1

    save(path, state)
    return 0


if __name__ == "__main__":
    sys.exit(main())
