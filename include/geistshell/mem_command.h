#ifndef GEISTSHELL_MEM_COMMAND_H
#define GEISTSHELL_MEM_COMMAND_H

#ifdef __cplusplus
extern "C" {
#endif

/* CLI `memory` subcommand. argv holds argc tokens after "memory":
 *   list
 *   read <slug>
 *   save <slug> <description>   (the body is read from stdin)
 *   delete <slug>
 * The store directory comes from --dir <path>, then $GEISTSHELL_MEMORY_DIR,
 * then ./memory. Returns a process exit code: 0 ok, 2 usage, non-zero on error. */
[[nodiscard]] int spg_memory_command(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif
