#ifndef LSP_H
#define LSP_H

#include <sys/types.h>
#include <unistd.h>

/* A single diagnostic (error or warning) reported by a language server. */
typedef struct {
  int line;          /* 0-based line number */
  int col;           /* 0-based character offset */
  int severity;      /* 1=Error, 2=Warning */
  char *message;     /* heap-allocated message string */
} Diagnostic;

/* Per-buffer LSP client state. */
typedef struct {
  int fd_in;                  /* write end of pipe → server stdin  */
  int fd_out;                 /* read end  of pipe ← server stdout */
  pid_t pid;                  /* server process id (-1 if none)     */
  char *server_cmd;           /* heap copy of the launch command    */
  int initialized;            /* 1 after initialize/initialized handshake */
  int req_id;                 /* monotonic id for JSON-RPC requests */
  int version;                /* document version counter (didChange) */
  int diag_error_count;       /* number of errors in diagnostics[]  */
  int diag_warning_count;     /* number of warnings in diagnostics[] */
  Diagnostic *diagnostics;    /* heap array of current diagnostics  */
  int diag_count;             /* used entries                       */
  int diag_cap;               /* allocated capacity                 */
  char *read_buf;             /* accumulation buffer for server output */
  int read_len;               /* bytes currently in read_buf        */
  int read_cap;               /* allocated capacity of read_buf     */
} LspClient;

/* Start the language server process, wiring up stdin/stdout pipes.
 * cmd is the command to execvp (space-separated tokens).
 * Returns 0 on success, -1 on error. */
int lsp_start(LspClient *client, const char *cmd);

/* Send the initialize / initialized handshake. */
void lsp_initialize(LspClient *client);

/* Notify the server that a document was opened. */
void lsp_didOpen(LspClient *client, const char *uri, const char *text);

/* Notify the server that a document changed (full-content sync). */
void lsp_didChange(LspClient *client, const char *uri, const char *text);

/* Non-blocking read of available server output; parses publishDiagnostics
 * notifications and updates client->diagnostics[]. */
void lsp_read(LspClient *client);

/* Kill the server process and free all resources. */
void lsp_free(LspClient *client);

/* Return the server launch command for a given Syntax->name, or NULL. */
const char *lsp_cmd_for_syntax(const char *syntax_name);

#endif /* LSP_H */
