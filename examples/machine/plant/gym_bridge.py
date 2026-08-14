#!/usr/bin/env python3
"""A Gymnasium environment behind exec channels.

The point is the author. heater.py is a machine I wrote, so a controller
written against it is graded by its own examiner. A Gymnasium environment
brings foreign physics, a foreign difficulty and — for the benchmark suites —
foreign published baselines to compare against.

geistshell needs no change for this: a channel is a program, so a simulated
industrial vessel, a real PLC behind an mbpoll wrapper and an RL benchmark are
the same code path with different channel tables. An environment is a stateful
process, so the bridge stays resident behind a Unix socket and the channel
programs are one-line clients it writes itself:

    build/gymenv/bin/python gym_bridge.py --env Pendulum-v1 \
        --socket build/gym.sock --channels-dir build/gym-plant

    geistshell device --config build/gym-plant/plant.spg read obs0

Channels (all values in HUNDREDTHS, saturated into int16, never wrapped):

    obs0..obsN   read     observation[i]
    reward       read     of the last committed step
    terminated   read     0/1
    truncated    read     0/1
    steps        read     saturates at 32767
    return       read     cumulative, saturating
    saturations  read     how often a value was pinned at the limit
    act0..actM   write    staged, not applied
    commit       write    1 applies the staged action and steps; 0 is a no-op
    reset        write    1 starts a new episode; 0 is a no-op

WHY A COMMIT CHANNEL. An RL environment advances only when acted upon, and a
multi-dimensional action arrives one channel at a time. Stepping on "the last
action channel was written" would make the meaning of a write depend on which
one it was — fine for Pendulum's single dimension, wrong the moment a
quadrotor with two arrives. One extra invocation buys an interface with no
ambiguity. 0 as an accepted no-op is deliberate: it is the safe value, so a
watchdog driving the table to safe stops the plant without stepping it.

That the environment steps on command rather than on a clock is a lucky fit:
the agent loop's own clock is a step counter (`step + 1`), which is what makes
its replay deterministic. The heater's wall-clock physics was the mismatch,
not the rule.
"""

import argparse
import os
import socket
import stat
import sys
import threading

INT16_MIN, INT16_MAX = -32768, 32767
SCALE = 100.0


class Env:
    """One environment, addressed as named channels.

    The lock covers everything: the socket server runs one thread per
    connection, and Gymnasium environments are not thread-safe.
    """

    def __init__(self, env_id, seed):
        import gymnasium

        self.env = gymnasium.make(env_id)
        self.seed = seed
        self.lock = threading.Lock()
        self.n_action = int(getattr(self.env.action_space, "shape", (1,))[0] or 1)
        self.staged = [0] * self.n_action
        self.saturations = 0
        self._reset()

    # --- scaling ---------------------------------------------------------
    def _to_reg(self, value):
        raw = int(round(float(value) * SCALE))
        if raw > INT16_MAX:
            self.saturations += 1
            return INT16_MAX
        if raw < INT16_MIN:
            self.saturations += 1
            return INT16_MIN
        return raw

    # --- episode ---------------------------------------------------------
    def _reset(self):
        # Seeded on every reset, not once at construction: two runs of the same
        # scripted agent must see the same episode, or a journal replay proves
        # nothing about the decisions in it.
        obs, _ = self.env.reset(seed=self.seed)
        self.obs = list(obs)
        self.reward = 0.0
        self.total = 0.0
        self.terminated = False
        self.truncated = False
        self.steps = 0
        self.staged = [0] * self.n_action

    def _step(self):
        if self.terminated or self.truncated:
            # Refused, not silently ignored. Gymnasium itself warns and returns
            # nonsense if you step a finished episode; a controller must be
            # able to tell "acted" from "the episode was already over".
            return None
        action = [v / SCALE for v in self.staged]
        space = self.env.action_space
        if getattr(space, "shape", None):
            import numpy

            act = numpy.clip(
                numpy.array(action, dtype=numpy.float32), space.low, space.high
            )
        else:
            act = int(self.staged[0] / SCALE)  # discrete: the index itself
        obs, reward, terminated, truncated, _ = self.env.step(act)
        self.obs = list(obs)
        self.reward = float(reward)
        self.total += self.reward
        self.terminated = bool(terminated)
        self.truncated = bool(truncated)
        self.steps += 1
        return 1

    # --- the channel interface ------------------------------------------
    def read(self, name):
        with self.lock:
            if name.startswith("obs"):
                index = int(name[3:])
                if index >= len(self.obs):
                    return None  # not a channel this environment has
                return self._to_reg(self.obs[index])
            if name == "reward":
                return self._to_reg(self.reward)
            if name == "terminated":
                return 1 if self.terminated else 0
            if name == "truncated":
                return 1 if self.truncated else 0
            if name == "steps":
                return min(self.steps, INT16_MAX)
            if name == "return":
                return self._to_reg(self.total)
            if name == "saturations":
                return min(self.saturations, INT16_MAX)
        return None

    def write(self, name, value):
        with self.lock:
            if name.startswith("act"):
                index = int(name[3:])
                if index >= self.n_action:
                    return None
                self.staged[index] = value
                return value
            if name == "commit":
                if value == 0:
                    return 0  # the safe value: stop commanding, step nothing
                if value != 1:
                    return None
                return value if self._step() is not None else None
            if name == "reset":
                if value == 0:
                    return 0
                if value != 1:
                    return None
                self._reset()
                return value
        return None


# --- the wire: one line in, one line out --------------------------------


def serve(sock_path, env, banner):
    if os.path.exists(sock_path):
        os.unlink(sock_path)
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    srv.bind(sock_path)
    srv.listen(8)
    print(banner, flush=True)
    while True:
        conn, _ = srv.accept()
        threading.Thread(target=_handle, args=(conn, env), daemon=True).start()


def _handle(conn, env):
    with conn, conn.makefile("rw", encoding="ascii", newline="\n") as f:
        for line in f:
            parts = line.split()
            result = None
            try:
                if len(parts) == 2 and parts[0] == "read":
                    result = env.read(parts[1])
                elif len(parts) == 3 and parts[0] == "write":
                    result = env.write(parts[1], int(parts[2]))
            except (ValueError, IndexError):
                result = None
            f.write("err\n" if result is None else f"ok {result}\n")
            f.flush()


def client(sock_path, name, value):
    """Channel-program mode: the exec contract, three lines of transport."""
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
        s.connect(sock_path)
        request = f"read {name}\n" if value is None else f"write {name} {value}\n"
        s.sendall(request.encode("ascii"))
        reply = s.makefile("r", encoding="ascii").readline().split()
    if not reply or reply[0] != "ok":
        return 1
    print(reply[1])
    return 0


# --- self-describing channel table --------------------------------------


def write_channels(directory, sock_path, bridge, env):
    """One script per channel plus the (device ...) table — the bridge knows
    its own observation and action dimensions, so nobody hand-counts them."""
    os.makedirs(directory, exist_ok=True)
    names = [f"obs{i}" for i in range(len(env.obs))]
    names += ["reward", "terminated", "truncated", "steps", "return",
              "saturations"]
    writable = [f"act{i}" for i in range(env.n_action)] + ["commit", "reset"]
    lines = ["(device"]
    for name in names + writable:
        path = os.path.join(directory, name)
        with open(path, "w", encoding="ascii") as f:
            f.write("#!/bin/sh\n"
                    f'exec {sys.executable} {bridge} --client {sock_path} '
                    f'{name} "$@"\n')
        os.chmod(path, os.stat(path).st_mode | stat.S_IXUSR | stat.S_IXGRP)
        safe = " (safe 0)" if name in writable else ""
        lines.append(f'  (channel (name "{name}") (program "{path}")'
                     f" (range {INT16_MIN} {INT16_MAX}){safe})")
    lines[-1] += ")"
    with open(os.path.join(directory, "plant.spg"), "w", encoding="ascii") as f:
        f.write("\n".join(lines) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--env", default="Pendulum-v1")
    parser.add_argument("--socket", default="build/gym.sock")
    parser.add_argument("--channels-dir", default="build/gym-plant")
    parser.add_argument(
        "--seed",
        type=int,
        default=0,
        help="reset seed; fixed so a replayed run meets the same episode",
    )
    parser.add_argument("--client", nargs="+", metavar=("SOCKET", "NAME"),
                        help=argparse.SUPPRESS)
    args = parser.parse_args()

    if args.client:
        sock, name = args.client[0], args.client[1]
        value = args.client[2] if len(args.client) > 2 else None
        sys.exit(client(sock, name, value))

    env = Env(args.env, args.seed)
    write_channels(args.channels_dir, os.path.abspath(args.socket),
                   os.path.abspath(__file__), env)
    serve(args.socket,
          env,
          f"env={args.env} obs={len(env.obs)} act={env.n_action} "
          f"channels={args.channels_dir}/plant.spg")


if __name__ == "__main__":
    main()
