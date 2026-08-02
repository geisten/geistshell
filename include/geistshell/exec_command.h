#ifndef GEISTSHELL_EXEC_COMMAND_H
#define GEISTSHELL_EXEC_COMMAND_H

#ifdef __cplusplus
extern "C" {
#endif

/* Entry point for the CLI `exec` subcommand. argv holds argc tokens: the
 * command to run and its arguments (argv[0] is the program name). Probes the
 * host, validates the command through the executor boundary, runs it under the
 * concurrent executor, prints an s-expression result summary plus the captured
 * stdout/stderr, and returns a process exit code:
 *   0   command ran and exited 0
 *   1.. the command's own non-zero exit code
 *   2   usage error (no command given)
 *   3   blocked by the executor boundary
 *   4   internal error / spawn failure / killed by signal */
[[nodiscard]] int spg_exec_command(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif
