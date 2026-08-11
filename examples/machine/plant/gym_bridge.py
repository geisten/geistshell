#!/usr/bin/env python3
"""A Gymnasium environment behind Modbus TCP.

The point is the author. heater.py is a machine I wrote, so a controller
written against it is graded by its own examiner. A Gymnasium environment
brings foreign physics, a foreign difficulty and — for the benchmark suites —
foreign published baselines to compare against.

geistshell needs no change for this: it already speaks Modbus, so a simulated
industrial vessel, a real OpenPLC runtime and an RL benchmark are the same code
path with different channel tables.

    build/gymenv/bin/python gym_bridge.py --env Pendulum-v1 --port 5502

Register map (holding, 16-bit signed, all values in HUNDREDTHS):

    0..31    observation[i]        read
    100      reward                read    of the last committed step
    101      terminated 0/1        read
    102      truncated  0/1        read
    103      step count            read    saturates at 32767
    104      return                read    cumulative, saturating
    200..231 action[i]             write   staged, not applied
    250      commit                write   1 applies the staged action, steps
    251      reset                 write   1 starts a new episode

WHY A COMMIT REGISTER. An RL environment advances only when acted upon, and a
multi-dimensional action arrives one register at a time. Stepping on "the last
action register was written" would make the meaning of a write depend on which
one it was — fine for Pendulum's single dimension, wrong the moment a quadrotor
with two arrives. One extra round trip buys an interface with no ambiguity.

That the environment steps on command rather than on a clock is a lucky fit:
the agent loop's own clock is a step counter (`step + 1`), which is what makes
its replay deterministic. The heater's wall-clock physics was the mismatch, not
the rule.

SCALING. Registers are 16-bit; observations are floats. Everything is carried
in hundredths and SATURATED, never wrapped — an observation that overflows into
a plausible small number is worse than one pinned at the limit, because the
first is silently wrong. Register 105 counts how often that happened, so a
channel table built on a badly scaled environment is visible rather than
mysterious.
"""

import argparse
import threading

import gymnasium
import modbus_server

OBS_BASE = 0
REWARD, TERMINATED, TRUNCATED, STEPS, RETURN, SATURATIONS = range(100, 106)
ACTION_BASE = 200
COMMIT, RESET = 250, 251

MAX_CHANNELS = 32
INT16_MIN, INT16_MAX = -32768, 32767
SCALE = 100.0


class Env:
    """One environment, addressed as registers.

    The lock covers everything: Modbus handlers run one thread per connection,
    and Gymnasium environments are not thread-safe.
    """

    def __init__(self, env_id, seed):
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

    # --- the register interface -----------------------------------------
    def read(self, register):
        with self.lock:
            if OBS_BASE <= register < OBS_BASE + MAX_CHANNELS:
                index = register - OBS_BASE
                if index >= len(self.obs):
                    return None  # not a channel this environment has
                return self._to_reg(self.obs[index])
            if register == REWARD:
                return self._to_reg(self.reward)
            if register == TERMINATED:
                return 1 if self.terminated else 0
            if register == TRUNCATED:
                return 1 if self.truncated else 0
            if register == STEPS:
                return min(self.steps, INT16_MAX)
            if register == RETURN:
                return self._to_reg(self.total)
            if register == SATURATIONS:
                return min(self.saturations, INT16_MAX)
        return None

    def write(self, register, value):
        with self.lock:
            if ACTION_BASE <= register < ACTION_BASE + MAX_CHANNELS:
                index = register - ACTION_BASE
                if index >= self.n_action:
                    return None
                self.staged[index] = value
                return value
            if register == COMMIT:
                if value != 1:
                    return None
                return value if self._step() is not None else None
            if register == RESET:
                if value != 1:
                    return None
                self._reset()
                return value
        return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--env", default="Pendulum-v1")
    parser.add_argument("--port", type=int, default=5502)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument(
        "--seed",
        type=int,
        default=0,
        help="reset seed; fixed so a replayed run meets the same episode",
    )
    args = parser.parse_args()

    env = Env(args.env, args.seed)
    print(
        f"env={args.env} obs={len(env.obs)} act={env.n_action}",
        flush=True,
    )
    modbus_server.serve(args.host, args.port, env, f"gym bridge {args.env}")


if __name__ == "__main__":
    main()
