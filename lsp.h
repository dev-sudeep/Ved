#ifndef LSP_H
#define LSP_H

#include <limits.h>
#include <stdint.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define MAX_LSP_FULL_SYNC_SIZE (512 * 1024) /* 512 KB threshold before using chunked/incremental sync */
#define LSP_CHUNK_SIZE (64 * 1024)          /* 64 KB chunk size for large document streaming */

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
  char *out_buf;              /* non-blocking outgoing queue to server stdin */
  int out_len;                /* bytes currently in out_buf         */
  int out_cap;                /* allocated capacity of out_buf      */
  uint64_t last_change_ms;    /* timestamp of last edit for debouncing */
  int is_dirty;               /* 1 if pending edit needs sending    */
  int has_range_edit;         /* 1 if pending edit is an incremental range edit */
  int dirty_start_line;       /* range edit start line */
  int dirty_start_col;        /* range edit start column */
  int dirty_end_line;         /* range edit end line */
  int dirty_end_col;          /* range edit end column */
} LspClient;

/* Start the language server process, wiring up stdin/stdout pipes.
 * cmd is the command to execvp (space-separated tokens).
 * Returns 0 on success, -1 on error. */
int lsp_start(LspClient *client, const char *cmd);

/* Send the initialize / initialized handshake with incremental sync capability. */
void lsp_initialize(LspClient *client);

/* Notify the server that a document was opened. */
void lsp_didOpen(LspClient *client, const char *uri, const char *text);

/* Stream large document didOpen in chunks for files > 512KB / 600KB. */
void lsp_didOpen_chunked(LspClient *client, const char *uri, const char *text);

/* Notify the server that a document changed (full-content sync). */
void lsp_didChange(LspClient *client, const char *uri, const char *text);

/* Notify the server of an incremental range edit (chunked diff). */
void lsp_didChange_range(LspClient *client, const char *uri, int start_line, int start_col, int end_line, int end_col, const char *inserted_text);

/* Flush pending outgoing bytes to server stdin (non-blocking). */
void lsp_flush(LspClient *client);

/* Non-blocking read of available server output; parses publishDiagnostics
 * notifications and updates client->diagnostics[]. */
void lsp_read(LspClient *client);

/* Kill the server process and free all resources. */
void lsp_free(LspClient *client);

/* Return the server launch command for a given Syntax->name, or NULL. */
const char *lsp_cmd_for_syntax(const char *syntax_name);

#endif /* LSP_H */
