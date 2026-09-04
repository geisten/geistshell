# Security Policy

geistshell is a **governed agent runtime**: it executes actions proposed by a
language model behind a mandatory policy gate, an executor boundary, an OS-level
process sandbox, and a tamper-evident audit log. This document states the threat
model honestly — **what the runtime enforces, and just as importantly what it
does not** — and how to report vulnerabilities.

> Status: **v0.1.0, pre-release.** The architecture is built for auditability and
> containment, but it has not had an independent security audit. Treat it as a
> research-grade control plane, not a hardened multi-tenant sandbox. Run it
> accordingly (see *Operator hardening*).

## Reporting a vulnerability

**Do not open a public GitHub issue for security problems.**

Email **g.schlegel@geisten.com** with:

- a description and the impact you believe it has,
- a minimal reproduction (config, policy, command, model output if relevant),
- the commit hash.

You will get an acknowledgement; please allow a reasonable window for a fix
before public disclosure. There is no bug-bounty program at this stage.

## Trust model

**Trusted** (the security of the system assumes these are honest/correct):

- the operator and the host OS / kernel,
- the build toolchain and the compiled binary,
- the **policy configuration** — this *is* the security boundary; a permissive
  policy is a permissive system,
- the external inference engine binary (libgeist) and its weights.

**Untrusted — treated as adversarial input:**

- every model **recommendation** (action kind, capability, `cost`,
  `uses_network`, command, memory body),
- responses from a **remote** model endpoint,
- the **stdout/stderr** of any executed command,
- anything previously written into the **mind-palace** (it is re-injected into
  later contexts).

The design assumption is simple: **the model is not trusted.** The gate,
boundary, and sandbox are the defense. If they are configured permissively, the
model effectively has whatever power they grant.

## What the runtime enforces

### 1. Mandatory policy gate (`src/policy`)

Every non-terminal action passes `spg_policy_gate_step` before any executor
runs; the orchestrator dispatches to an executor **only** on an `ALLOW`
decision. The gate checks, in order:

- the action's **capability** name must exist in the policy and be **enabled**
  (capabilities are opt-in; an unknown or disabled capability is denied),
- the capability **kind** must match the action kind,
- **network**: if `uses_network` is set and the policy's `network_default` is
  `DENY`, the action is denied (except `ssh_auth_probe`). For a `device_write`
  the flag is derived from the target channel's operator-declared
  `(network true)` field in the loaded table (#119) — never from model text
  (the recommendation form must say `uses_network false` or it fails to
  parse). Declaring a channel's transport truthfully is operator trust:
  geistshell cannot verify what the channel program does once it runs,
- **budgets**: per-capability budget and global per-action-class budget; usage is
  accumulated monotonically across the run.

The decision is journaled (`policy_decision` event) whether ALLOW or DENY.

### 2. Executor boundary (`src/executor/executor_boundary.c`)

A second, independent check before the shell runs. It denies unless: execution
is enabled, the policy decision is ALLOW, the action is `local_shell`,
`uses_network` is false, a command is present, the working directory is under
the configured `allowed_workdir_prefix`, and the timeout / output limits are
within the configured maxima.

### 3. OS process sandbox (`src/exec/cmd_executor.c`)

Shell commands run via **`fork` + `execvp`** with:

- **No shell.** The command is split on whitespace and the program is exec'd
  directly. Shell metacharacters (`|`, `>`, `;`, `&&`, backticks, `$()`) are
  **not** interpreted — they become literal arguments. This removes the classic
  shell-injection surface: there is no `system()` / `sh -c`.
- **Own process group** (`setpgid`): on timeout the whole subtree is killed
  (`kill(-pgid, SIGKILL)`), so a child that spawns grandchildren cannot outlive
  the deadline.
- **Wall-clock timeout** → `SIGKILL`.
- **Resource limits** via `setrlimit` (best-effort): CPU time (default 30 s),
  address space (default 2 GiB), file size written (default 64 MiB). `RLIMIT_AS`
  is not reliably enforced on macOS; `RLIMIT_NPROC` is left unset by default.
- **stdin = `/dev/null`**; stdout/stderr captured through `O_CLOEXEC`,
  non-blocking pipes and **byte-capped** (truncation flagged), so a runaway
  writer cannot exhaust memory.
- Execution is **opt-in** (`execution_enabled`); when off, the boundary denies
  with `execution_disabled`. Default deployments do not run shell commands.

### 4. Tamper-evident audit log (`src/journal`, `src/core/hash.c`)

Every step — model input, model output, policy decision, action, result — is
appended to a **BLAKE3 hash-chained** journal with logical timestamps, enabling
**byte-identical replay**. An optional **keyed HMAC seal** provides symmetric
tamper-evidence. (Asymmetric Ed25519 signatures are on the roadmap, not shipped.)

### 5. Secret handling

The remote API key is read **only** from the `GEISTSHELL_API_KEY` environment
variable, used solely to build the `Authorization` header, and is **never** a
command-line argument and **never** written to the journal.

### 6. Path safety (mind-palace)

Memory slugs are validated to `[a-z0-9-]`, 1–64 bytes — no path separators,
dots, or traversal. All file paths are built from a validated slug, never from
raw model text. Writes are atomic (temp file + `rename`).

## What the runtime does NOT do

These are deliberate scope limits. **Read them before exposing geistshell to an
untrusted model or running it on a machine you care about.**

- **No filesystem jail.** The sandbox `chdir`s into the working directory but
  there is **no `chroot` / mount namespace**. An executed command runs with the
  launching process's full uid/gid and can read or write anything those
  privileges allow, via absolute paths or `..`. `allowed_workdir_prefix`
  constrains the *launch* directory, **not** what the command can touch.
- **No network enforcement at the syscall level.** Network containment is
  *declarative*: it relies on the model's self-declared `uses_network` flag
  (for device channels: the operator-declared `(network ...)` field of the
  channel table, #119) and the policy `network_default`. The command itself is **not** inspected, and the
  OS does not block sockets. A program that does network I/O (e.g. `curl`, `nc`)
  launched with `uses_network=false` will **not** be stopped by geistshell.
- **No privilege drop, no seccomp, no cgroups.** The child inherits the parent's
  uid/gid/groups; there is no syscall filtering and no `capabilities(7)`
  reduction. Resource limits are `setrlimit` only (best-effort, platform-
  dependent).
- **The journal is not confidential.** It records prompts, model outputs, and
  tool results in cleartext. Any secret that appears in the model's context or
  output is persisted to disk. Only the API key is excluded. The HMAC seal
  protects *integrity*, not *confidentiality*.
- **Remote inference is data egress.** With `--remote-url`, the assembled
  context (which can include mind-palace contents, prior outputs, and tool
  results) is sent to the configured third-party endpoint.
- **Prompt-injection can persist.** A malicious tool result or model output can
  be saved into the mind-palace and re-injected into future contexts (including
  via the self-improvement loop). The eval gate guards against pass-rate
  *regressions*, not against injected *instructions*.

## Operator hardening

geistshell provides *governance and audit*, not OS isolation. For untrusted or
high-stakes use, layer it under real isolation:

1. **Run as a dedicated, unprivileged user** — never as root.
2. **Put it in a container, VM, or jail** for the filesystem and network
   isolation the runtime does not provide. For network denial, use a network
   namespace / egress firewall — do **not** rely on the policy network flag.
3. **Keep `execution_enabled` off** unless you need shell actions. When on, point
   `allowed_workdir_prefix` at a disposable scratch directory and set tight
   timeouts, output caps, and `setrlimit` values.
4. **Treat the policy config as the security boundary.** Enable the minimum
   capabilities, set conservative budgets, keep `network_default = deny`.
5. **Protect the journal at rest** (it contains prompts/outputs); store the HMAC
   seal key outside the journal directory.
6. **Keep secrets out of the model context.** Provide the API key via the
   environment only.

## Supported versions

| Version | Supported |
| ------- | --------- |
| 0.1.x   | Best-effort; pre-release, no security SLA |

Fixes land on `main`; there is no backport guarantee yet.
