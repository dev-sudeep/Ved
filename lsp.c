/*
 * lsp.c — Language Server Protocol client for ved
 *
 * Implements JSON-RPC 2.0 over subprocess pipes (stdin/stdout).
 * No external dependencies; uses hand-written minimal JSON builder/parser.
 *
 * Build:  gcc -O2 -Wall -o ved ved.c lsp.c -lutil
 *
 * Supported servers:
 *   C/C++          clangd
 *   Python         pylsp  (python-lsp-server)
 *   JavaScript/TS  typescript-language-server --stdio
 *   Shell          bash-language-server start
 *   Rust           rust-analyzer
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE

#include "lsp.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* ================================================================
 * Minimal JSON builder helpers
 * ================================================================ */

/**
 * Escape a raw C string for safe inclusion inside a JSON string value.
 * The result is written into `out` (NUL-terminated, at most outsz-1 bytes).
 */
static void json_escape(char *out, int outsz, const char *s) {
    int o = 0;
    if (!s) { if (outsz > 0) out[0] = '\0'; return; }
    for (int i = 0; s[i] && o < outsz - 4; i++) {
        unsigned char c = (unsigned char)s[i];
        if      (c == '"')  { out[o++] = '\\'; out[o++] = '"';  }
        else if (c == '\\') { out[o++] = '\\'; out[o++] = '\\'; }
        else if (c == '\n') { out[o++] = '\\'; out[o++] = 'n';  }
        else if (c == '\r') { out[o++] = '\\'; out[o++] = 'r';  }
        else if (c == '\t') { out[o++] = '\\'; out[o++] = 't';  }
        else if (c < 0x20)  { /* other control chars: skip      */ }
        else                { out[o++] = (char)c;                }
    }
    out[o] = '\0';
}

/* ================================================================
 * Minimal JSON parser (for receiving LSP responses)
 * ================================================================ */

/* Skip whitespace, return updated pointer. */
static const char *jskip(const char *p) {
    if (!p) return p;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

/**
 * Skip over one complete JSON value starting at p.
 * Works for strings, objects {}, arrays [], and scalars.
 * Returns pointer just past the end of the value.
 */
static const char *jskip_value(const char *p) {
    if (!p) return p;
    p = jskip(p);
    if (!*p) return p;

    if (*p == '"') {
        p++;
        while (*p && *p != '"') { if (*p == '\\') p++; if (*p) p++; }
        if (*p == '"') p++;
        return p;
    }
    if (*p == '{') {
        int d = 0;
        while (*p) {
            if      (*p == '"') { p++; while (*p && *p != '"') { if (*p=='\\') p++; if (*p) p++; } if (*p) p++; }
            else if (*p == '{') { d++; p++; }
            else if (*p == '}') { d--; p++; if (d == 0) return p; }
            else                { p++; }
        }
        return p;
    }
    if (*p == '[') {
        int d = 0;
        while (*p) {
            if      (*p == '"') { p++; while (*p && *p != '"') { if (*p=='\\') p++; if (*p) p++; } if (*p) p++; }
            else if (*p == '[') { d++; p++; }
            else if (*p == ']') { d--; p++; if (d == 0) return p; }
            else                { p++; }
        }
        return p;
    }
    /* scalar: number / bool / null */
    while (*p && *p != ',' && *p != '}' && *p != ']' && *p != ' '
                && *p != '\t' && *p != '\n' && *p != '\r') p++;
    return p;
}

/**
 * Find the value for `key` inside the JSON object that p points to ('{...}').
 * Only searches immediate (depth-1) keys.
 * Returns a pointer to the start of the value, or NULL if not found.
 */
static const char *jfind(const char *p, const char *key) {
    if (!p || !key) return NULL;
    p = jskip(p);
    if (*p != '{') return NULL;
    p++; /* skip opening '{' */

    char needle[256];
    int nlen = snprintf(needle, sizeof(needle), "\"%s\"", key);

    while (*p) {
        p = jskip(p);
        if (*p == '}' || *p == '\0') return NULL;
        if (*p != '"') return NULL; /* malformed JSON */

        /* Check whether this key matches */
        if (strncmp(p, needle, (size_t)nlen) == 0) {
            const char *after = jskip(p + nlen);
            if (*after == ':') {
                return jskip(after + 1);
            }
        }
        /* Skip the key string */
        p++;
        while (*p && *p != '"') { if (*p == '\\') p++; if (*p) p++; }
        if (*p == '"') p++;

        p = jskip(p);
        if (*p != ':') return NULL; /* malformed */
        p++;

        /* Skip the value */
        p = jskip_value(p);
        p = jskip(p);
        if (*p == ',') p++;
    }
    return NULL;
}

/**
 * Parse an integer starting at p (may be preceded by whitespace).
 * If endp != NULL, sets *endp to the character after the number.
 */
static int jint(const char *p, const char **endp) {
    if (!p) { if (endp) *endp = p; return 0; }
    p = jskip(p);
    int neg = (*p == '-') ? (p++, 1) : 0;
    int v = 0;
    while (isdigit((unsigned char)*p)) v = v * 10 + (*p++ - '0');
    if (endp) *endp = p;
    return neg ? -v : v;
}

/**
 * Parse a JSON string value.  p must point AFTER the opening '"'.
 * Copies at most outsz-1 decoded characters into out.
 * Returns pointer just after the closing '"'.
 */
static const char *jstr(const char *p, char *out, int outsz) {
    int o = 0;
    if (!p) { if (out && outsz > 0) out[0] = '\0'; return p; }
    while (*p && *p != '"' && o < outsz - 1) {
        if (*p == '\\') {
            p++;
            switch (*p) {
            case '"':  out[o++] = '"';  break;
            case '\\': out[o++] = '\\'; break;
            case '/':  out[o++] = '/';  break;
            case 'n':  out[o++] = '\n'; break;
            case 'r':  out[o++] = '\r'; break;
            case 't':  out[o++] = '\t'; break;
            default:   if (*p) out[o++] = *p; break;
            }
        } else {
            out[o++] = *p;
        }
        if (*p) p++;
    }
    if (out) out[o] = '\0';
    if (*p == '"') p++;
    return p;
}

/* ================================================================
 * Send helpers
 * ================================================================ */

void lsp_flush(LspClient *c) {
    if (!c || c->fd_in < 0 || c->out_len <= 0) return;
    ssize_t w = write(c->fd_in, c->out_buf, (size_t)c->out_len);
    if (w > 0) {
        if (w < c->out_len) {
            memmove(c->out_buf, c->out_buf + w, (size_t)(c->out_len - w));
            c->out_len -= (int)w;
        } else {
            c->out_len = 0;
        }
    } else if (w < 0 && (errno == EPIPE || errno == EBADF)) {
        close(c->fd_in);  c->fd_in = -1;
        if (c->fd_out >= 0) { close(c->fd_out); c->fd_out = -1; }
        c->initialized = 0;
        c->out_len = 0;
    }
}

static void lsp_send_raw(LspClient *c, const char *body) {
    if (!c || c->fd_in < 0 || !body) return;
    int blen = (int)strlen(body);
    char header[64];
    int hlen = snprintf(header, sizeof(header),
                        "Content-Length: %d\r\n\r\n", blen);
    int total_len = hlen + blen;

    /* Flush existing queue if any */
    if (c->out_len > 0) {
        lsp_flush(c);
    }

    int written_bytes = 0;
    if (c->out_len == 0) {
        ssize_t w1 = write(c->fd_in, header, (size_t)hlen);
        if (w1 > 0) {
            if (w1 == hlen) {
                ssize_t w2 = write(c->fd_in, body, (size_t)blen);
                if (w2 > 0) {
                    written_bytes = hlen + (int)w2;
                } else if (w2 < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                    if (errno == EPIPE || errno == EBADF) {
                        close(c->fd_in); c->fd_in = -1;
                        if (c->fd_out >= 0) { close(c->fd_out); c->fd_out = -1; }
                        c->initialized = 0;
                    }
                    return;
                } else {
                    written_bytes = hlen;
                }
            } else {
                written_bytes = (int)w1;
            }
        } else if (w1 < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            if (errno == EPIPE || errno == EBADF) {
                close(c->fd_in); c->fd_in = -1;
                if (c->fd_out >= 0) { close(c->fd_out); c->fd_out = -1; }
                c->initialized = 0;
            }
            return;
        }
    }

    /* Queue unwritten remainder to out_buf */
    if (written_bytes < total_len) {
        int rem = total_len - written_bytes;
        int need = c->out_len + rem;
        if (need > c->out_cap) {
            int newcap = c->out_cap ? c->out_cap * 2 : 4096;
            if (newcap < need) newcap = need;
            char *nb = realloc(c->out_buf, (size_t)newcap);
            if (!nb) return;
            c->out_buf = nb;
            c->out_cap = newcap;
        }
        if (written_bytes < hlen) {
            memcpy(c->out_buf + c->out_len, header + written_bytes, (size_t)(hlen - written_bytes));
            c->out_len += (hlen - written_bytes);
            memcpy(c->out_buf + c->out_len, body, (size_t)blen);
            c->out_len += blen;
        } else {
            int body_written = written_bytes - hlen;
            memcpy(c->out_buf + c->out_len, body + body_written, (size_t)(blen - body_written));
            c->out_len += (blen - body_written);
        }
    }
}

/* Send a JSON-RPC notification (no id). params may be NULL → "{}". */
static void lsp_notify(LspClient *c, const char *method, const char *params) {
    if (!params) params = "{}";
    int sz = (int)(strlen(method) + strlen(params)) + 64;
    if (sz < 256) sz = 256;
    char *buf = malloc((size_t)sz);
    if (!buf) return;
    snprintf(buf, (size_t)sz,
             "{\"jsonrpc\":\"2.0\",\"method\":\"%s\",\"params\":%s}",
             method, params);
    lsp_send_raw(c, buf);
    free(buf);
}

/* Send a JSON-RPC request (with a monotonically-increasing id). */
static void lsp_request(LspClient *c, const char *method, const char *params) {
    if (!params) params = "{}";
    int id  = ++c->req_id;
    int sz  = (int)(strlen(method) + strlen(params)) + 80;
    if (sz < 256) sz = 256;
    char *buf = malloc((size_t)sz);
    if (!buf) return;
    snprintf(buf, (size_t)sz,
             "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"%s\",\"params\":%s}",
             id, method, params);
    lsp_send_raw(c, buf);
    free(buf);
}

/* ================================================================
 * Public API
 * ================================================================ */

int lsp_start(LspClient *client, const char *cmd) {
    if (!client || !cmd) return -1;
    signal(SIGPIPE, SIG_IGN);
    memset(client, 0, sizeof(*client));
    client->fd_in = client->fd_out = -1;
    client->pid   = -1;

    int in_pipe[2], out_pipe[2];
    if (pipe(in_pipe)  < 0) return -1;
    if (pipe(out_pipe) < 0) { close(in_pipe[0]); close(in_pipe[1]); return -1; }

    pid_t pid = fork();
    if (pid < 0) {
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        return -1;
    }

    if (pid == 0) {
        /* ---- child: wire pipes ---- */
        if (dup2(in_pipe[0],  STDIN_FILENO)  < 0) _exit(1);
        if (dup2(out_pipe[1], STDOUT_FILENO) < 0) _exit(1);
        /* Silence the server's stderr so it doesn't paint over the TUI */
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, STDERR_FILENO); close(devnull); }
        
        /* Close all other inherited file descriptors to prevent descriptor leakage */
        for (int fd = 3; fd < 256; fd++)
            close(fd);

        /* Tokenise command string */
        char cmdcopy[1024];
        strncpy(cmdcopy, cmd, sizeof(cmdcopy) - 1);
        cmdcopy[sizeof(cmdcopy) - 1] = '\0';
        char *argv_arr[64];
        int  argc = 0;
        char *tok = strtok(cmdcopy, " \t");
        while (tok && argc < 63) { argv_arr[argc++] = tok; tok = strtok(NULL, " \t"); }
        argv_arr[argc] = NULL;
        if (argc == 0) _exit(1);
        execvp(argv_arr[0], argv_arr);
        _exit(127); /* server binary not found */
    }

    /* ---- parent ---- */
    close(in_pipe[0]);
    close(out_pipe[1]);
    client->fd_in  = in_pipe[1];   /* write → child stdin  */
    client->fd_out = out_pipe[0];  /* read  ← child stdout */
    client->pid    = pid;

    /* Set FD_CLOEXEC so pipes are not inherited by future sub-forks */
    fcntl(client->fd_in,  F_SETFD, FD_CLOEXEC);
    fcntl(client->fd_out, F_SETFD, FD_CLOEXEC);

    /* Non-blocking reads and writes so the TUI never stalls waiting for the server */
    int fl_out = fcntl(client->fd_out, F_GETFL, 0);
    if (fl_out >= 0) fcntl(client->fd_out, F_SETFL, fl_out | O_NONBLOCK);

    int fl_in = fcntl(client->fd_in, F_GETFL, 0);
    if (fl_in >= 0) fcntl(client->fd_in, F_SETFL, fl_in | O_NONBLOCK);

    client->server_cmd = strdup(cmd);
    return 0;
}

void lsp_initialize(LspClient *client) {
    if (!client || client->fd_in < 0) return;

    char cwd[PATH_MAX];
    char root_uri[PATH_MAX + 32];
    if (getcwd(cwd, sizeof(cwd))) {
        snprintf(root_uri, sizeof(root_uri), "\"file://%s\"", cwd);
    } else {
        snprintf(root_uri, sizeof(root_uri), "null");
    }

    char params[2048];
    snprintf(params, sizeof(params),
        "{"
          "\"processId\":%d,"
          "\"clientInfo\":{\"name\":\"ved\",\"version\":\"0.1\"},"
          "\"rootUri\":%s,"
          "\"capabilities\":{"
            "\"textDocument\":{"
              "\"publishDiagnostics\":{\"relatedInformation\":false},"
              "\"synchronization\":{"
                "\"dynamicRegistration\":false,"
                "\"willSave\":false,"
                "\"didSave\":true,"
                "\"willSaveWaitUntil\":false,"
                "\"change\":2"
              "}"
            "}"
          "},"
          "\"trace\":\"off\","
          "\"workspaceFolders\":null"
        "}",
        (int)getpid(), root_uri);

    lsp_request(client, "initialize", params);
    /* Send 'initialized' immediately; most servers tolerate the ordering. */
    lsp_notify(client, "initialized", "{}");
    client->initialized = 1;
}

/* Determine the LSP languageId from the document URI (by extension). */
static const char *lang_id_from_uri(const char *uri) {
    if (!uri) return "plaintext";
    const char *ext = strrchr(uri, '.');
    if (!ext) return "plaintext";
    if (!strcmp(ext, ".c"))                                    return "c";
    if (!strcmp(ext, ".h"))                                    return "c";
    if (!strcmp(ext, ".cpp") || !strcmp(ext, ".cxx") ||
        !strcmp(ext, ".cc")  || !strcmp(ext, ".hpp"))          return "cpp";
    if (!strcmp(ext, ".py")  || !strcmp(ext, ".pyw"))          return "python";
    if (!strcmp(ext, ".js")  || !strcmp(ext, ".jsx") ||
        !strcmp(ext, ".mjs"))                                  return "javascript";
    if (!strcmp(ext, ".ts")  || !strcmp(ext, ".tsx"))          return "typescript";
    if (!strcmp(ext, ".rs"))                                   return "rust";
    if (!strcmp(ext, ".sh")  || !strcmp(ext, ".bash"))         return "shellscript";
    return "plaintext";
}

void lsp_didOpen(LspClient *client, const char *uri, const char *text) {
    if (!client || !client->initialized || client->fd_in < 0) return;
    if (!uri || !text) return;

    size_t tlen = strlen(text);
    if (tlen > MAX_LSP_FULL_SYNC_SIZE) {
        lsp_didOpen_chunked(client, uri, text);
        return;
    }

    const char *langid = lang_id_from_uri(uri);
    size_t elen = tlen * 2 + 8;
    size_t ulen = strlen(uri) * 2 + 8;

    char *etext = malloc(elen);
    char *euri  = malloc(ulen);
    if (!etext || !euri) { free(etext); free(euri); return; }
    json_escape(etext, (int)elen, text);
    json_escape(euri,  (int)ulen, uri);

    size_t psz = elen + ulen + 256;
    char *params = malloc(psz);
    if (!params) { free(etext); free(euri); return; }

    snprintf(params, psz,
        "{\"textDocument\":{"
          "\"uri\":\"%s\","
          "\"languageId\":\"%s\","
          "\"version\":%d,"
          "\"text\":\"%s\""
        "}}",
        euri, langid, ++client->version, etext);

    lsp_notify(client, "textDocument/didOpen", params);
    free(etext); free(euri); free(params);
}

void lsp_didChange_range(LspClient *client, const char *uri, int start_line, int start_col, int end_line, int end_col, const char *inserted_text) {
    if (!client || !client->initialized || client->fd_in < 0) return;
    if (!uri || !inserted_text) return;

    size_t tlen = strlen(inserted_text);
    size_t elen = tlen * 2 + 8;
    size_t ulen = strlen(uri) * 2 + 8;

    char *etext = malloc(elen);
    char *euri  = malloc(ulen);
    if (!etext || !euri) { free(etext); free(euri); return; }
    json_escape(etext, (int)elen, inserted_text);
    json_escape(euri,  (int)ulen, uri);

    size_t psz = elen + ulen + 512;
    char *params = malloc(psz);
    if (!params) { free(etext); free(euri); return; }

    snprintf(params, psz,
        "{\"textDocument\":{"
          "\"uri\":\"%s\","
          "\"version\":%d"
        "},"
        "\"contentChanges\":[{"
          "\"range\":{"
            "\"start\":{\"line\":%d,\"character\":%d},"
            "\"end\":{\"line\":%d,\"character\":%d}"
          "},"
          "\"text\":\"%s\""
        "}]}",
        euri, ++client->version, start_line, start_col, end_line, end_col, etext);

    lsp_notify(client, "textDocument/didChange", params);
    free(etext); free(euri); free(params);
}

void lsp_didOpen_chunked(LspClient *client, const char *uri, const char *text) {
    if (!client || !client->initialized || client->fd_in < 0) return;
    if (!uri || !text) return;

    size_t tlen = strlen(text);
    if (tlen <= MAX_LSP_FULL_SYNC_SIZE) {
        lsp_didOpen(client, uri, text);
        return;
    }

    char *chunk = malloc(LSP_CHUNK_SIZE + 1);
    if (!chunk) return;

    size_t offset = 0;
    size_t chunk_len = tlen > LSP_CHUNK_SIZE ? LSP_CHUNK_SIZE : tlen;
    memcpy(chunk, text, chunk_len);
    chunk[chunk_len] = '\0';

    /* Send first chunk via didOpen */
    const char *langid = lang_id_from_uri(uri);
    size_t elen = chunk_len * 2 + 8;
    size_t ulen = strlen(uri) * 2 + 8;
    char *etext = malloc(elen);
    char *euri  = malloc(ulen);
    if (etext && euri) {
        json_escape(etext, (int)elen, chunk);
        json_escape(euri,  (int)ulen, uri);
        size_t psz = elen + ulen + 256;
        char *params = malloc(psz);
        if (params) {
            snprintf(params, psz,
                "{\"textDocument\":{"
                  "\"uri\":\"%s\","
                  "\"languageId\":\"%s\","
                  "\"version\":%d,"
                  "\"text\":\"%s\""
                "}}",
                euri, langid, ++client->version, etext);
            lsp_notify(client, "textDocument/didOpen", params);
            free(params);
        }
        free(etext); free(euri);
    }
    offset += chunk_len;

    int current_line = 0;
    int current_col = 0;
    for (size_t i = 0; i < chunk_len; i++) {
        if (chunk[i] == '\n') {
            current_line++;
            current_col = 0;
        } else {
            current_col++;
        }
    }

    /* Stream remaining chunks via incremental didChange range appends */
    while (offset < tlen) {
        chunk_len = (tlen - offset > LSP_CHUNK_SIZE) ? LSP_CHUNK_SIZE : (tlen - offset);
        memcpy(chunk, text + offset, chunk_len);
        chunk[chunk_len] = '\0';

        lsp_didChange_range(client, uri, current_line, current_col, current_line, current_col, chunk);
        lsp_flush(client);

        for (size_t i = 0; i < chunk_len; i++) {
            if (chunk[i] == '\n') {
                current_line++;
                current_col = 0;
            } else {
                current_col++;
            }
        }
        offset += chunk_len;
    }

    free(chunk);
}

void lsp_didChange(LspClient *client, const char *uri, const char *text) {
    if (!client || !client->initialized || client->fd_in < 0) return;
    if (!uri || !text) return;

    size_t tlen = strlen(text);
    size_t elen = tlen * 2 + 8;
    size_t ulen = strlen(uri) * 2 + 8;

    char *etext = malloc(elen);
    char *euri  = malloc(ulen);
    if (!etext || !euri) { free(etext); free(euri); return; }
    json_escape(etext, (int)elen, text);
    json_escape(euri,  (int)ulen, uri);

    size_t psz = elen + ulen + 256;
    char *params = malloc(psz);
    if (!params) { free(etext); free(euri); return; }

    snprintf(params, psz,
        "{\"textDocument\":{"
          "\"uri\":\"%s\","
          "\"version\":%d"
        "},"
        "\"contentChanges\":[{\"text\":\"%s\"}]}",
        euri, ++client->version, etext);

    lsp_notify(client, "textDocument/didChange", params);
    free(etext); free(euri); free(params);
}

/* ================================================================
 * Dispatch incoming server messages
 * ================================================================ */

static void lsp_dispatch(LspClient *client, const char *body) {
    /* Notifications contain "method"; request responses do not. */
    const char *method_v = jfind(body, "method");
    if (!method_v || *method_v != '"') return;

    char method[128];
    jstr(method_v + 1, method, sizeof(method));
    if (strcmp(method, "textDocument/publishDiagnostics") != 0) return;

    const char *params_v = jfind(body, "params");
    if (!params_v) return;
    const char *diags_v  = jfind(params_v, "diagnostics");
    if (!diags_v || *diags_v != '[') return;

    /* Clear old diagnostics */
    for (int i = 0; i < client->diag_count; i++) {
        free(client->diagnostics[i].message);
        client->diagnostics[i].message = NULL;
    }
    client->diag_count         = 0;
    client->diag_error_count   = 0;
    client->diag_warning_count = 0;

    /* Walk the diagnostics array */
    const char *p = diags_v + 1; /* skip '[' */
    while (*p) {
        p = jskip(p);
        if (*p == ']' || *p == '\0') break;
        if (*p != '{') { p++; continue; } /* skip unexpected chars */

        const char *obj_start = p;
        const char *obj_end   = jskip_value(p);

        int olen = (int)(obj_end - obj_start);
        char *obj = malloc((size_t)olen + 1);
        if (!obj) break;
        memcpy(obj, obj_start, (size_t)olen);
        obj[olen] = '\0';

        /* Severity (default: Error) */
        int severity = 1;
        const char *sv = jfind(obj, "severity");
        if (sv) severity = jint(sv, NULL);
        if (severity < 1 || severity > 4) severity = 1;

        /* Line and column from range.start */
        int line = 0, col = 0;
        const char *range_v = jfind(obj, "range");
        if (range_v) {
            const char *start_v = jfind(range_v, "start");
            if (start_v) {
                const char *lv = jfind(start_v, "line");
                if (lv) line = jint(lv, NULL);
                const char *cv = jfind(start_v, "character");
                if (cv) col  = jint(cv, NULL);
            }
        }

        /* Message */
        char msg[512] = "";
        const char *mv = jfind(obj, "message");
        if (mv && *mv == '"') jstr(mv + 1, msg, sizeof(msg));
        free(obj);

        /* Only store errors (1) and warnings (2); skip hints/info (3/4) */
        if (severity == 1 || severity == 2) {
            if (client->diag_count >= client->diag_cap) {
                int newcap = client->diag_cap ? client->diag_cap * 2 : 32;
                Diagnostic *nd = realloc(client->diagnostics,
                                         sizeof(Diagnostic) * (size_t)newcap);
                if (!nd) break;
                client->diagnostics = nd;
                client->diag_cap    = newcap;
            }
            Diagnostic *d = &client->diagnostics[client->diag_count++];
            d->line     = line;
            d->col      = col;
            d->severity = severity;
            d->message  = strdup(msg[0] ? msg : "(no message)");
            if (severity == 1) client->diag_error_count++;
            else               client->diag_warning_count++;
        }

        p = obj_end;
        p = jskip(p);
        if (*p == ',') p++;
    }
}

/* ================================================================
 * Non-blocking read / message extraction
 * ================================================================ */

void lsp_read(LspClient *client) {
    /* fd_out < 3 guard: never read from stdin/stdout/stderr */
    if (!client || client->fd_out < 3) return;

    /* Append all available bytes to the internal read buffer */
    char tmp[8192];
    ssize_t n;
    while ((n = read(client->fd_out, tmp, sizeof(tmp))) > 0) {
        int need = client->read_len + (int)n + 1;
        if (need > client->read_cap) {
            int newcap = client->read_cap * 2;
            if (newcap < need * 2) newcap = need * 2;
            if (newcap < 8192)    newcap = 8192;
            char *nb = realloc(client->read_buf, (size_t)newcap);
            if (!nb) return;
            client->read_buf = nb;
            client->read_cap = newcap;
        }
        memcpy(client->read_buf + client->read_len, tmp, (size_t)n);
        client->read_len += (int)n;
        client->read_buf[client->read_len] = '\0';
    }

    /* EOF or hard error → server has exited */
    if (n == 0 ||
        (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
        close(client->fd_out);   client->fd_out = -1;
        if (client->fd_in >= 3) { close(client->fd_in); client->fd_in = -1; }
        else                      client->fd_in = -1;
        client->initialized = 0;
        return;
    }

    if (!client->read_buf || client->read_len <= 0) return;

    /* Extract and dispatch complete Content-Length-framed messages without O(N^2) memmove */
    int read_offset = 0;
    while (1) {
        char *cl = strstr(client->read_buf + read_offset, "Content-Length: ");
        if (!cl) break;
        int clen = atoi(cl + 16);
        if (clen <= 0) break;

        char *hdr_end = strstr(cl, "\r\n\r\n");
        if (!hdr_end) break;
        hdr_end += 4;

        int msg_offset = (int)(hdr_end - client->read_buf);
        if (client->read_len - msg_offset < clen) break; /* body incomplete */

        /* NUL-terminate the body temporarily */
        char saved = hdr_end[clen];
        hdr_end[clen] = '\0';
        lsp_dispatch(client, hdr_end);
        hdr_end[clen] = saved;

        read_offset = msg_offset + clen;
    }

    if (read_offset > 0) {
        int rem = client->read_len - read_offset;
        if (rem > 0) {
            memmove(client->read_buf, client->read_buf + read_offset, (size_t)rem);
        }
        client->read_len = rem;
        client->read_buf[client->read_len] = '\0';
    }
}

/* ================================================================
 * Cleanup
 * ================================================================ */

void lsp_free(LspClient *client) {
    if (!client) return;

    /* Safety: never close stdin (0), stdout (1), or stderr (2) */
    if (client->fd_in  >= 3) { close(client->fd_in);  client->fd_in  = -1; }
    else                       client->fd_in  = -1;
    if (client->fd_out >= 3) { close(client->fd_out); client->fd_out = -1; }
    else                       client->fd_out = -1;

    if (client->pid > 0) {
        kill(client->pid, SIGTERM);
        usleep(5000); /* 5ms grace period */
        if (waitpid(client->pid, NULL, WNOHANG) <= 0) {
            kill(client->pid, SIGKILL);
            waitpid(client->pid, NULL, 0);
        }
        client->pid = -1;
    }

    free(client->read_buf);  client->read_buf = NULL;
    client->read_len = client->read_cap = 0;
    free(client->out_buf);   client->out_buf = NULL;
    client->out_len = client->out_cap = 0;
    client->last_change_ms = 0;
    client->is_dirty = 0;
    free(client->server_cmd); client->server_cmd = NULL;

    for (int i = 0; i < client->diag_count; i++)
        free(client->diagnostics[i].message);
    free(client->diagnostics); client->diagnostics = NULL;
    client->diag_count = client->diag_cap = 0;

    client->initialized        = 0;
    client->diag_error_count   = 0;
    client->diag_warning_count = 0;
    client->version            = 0;
    client->req_id             = 0;
}

/* ================================================================
 * Language → server-command mapping
 * ================================================================ */

const char *lsp_cmd_for_syntax(const char *syntax_name) {
    if (!syntax_name) return NULL;
    /* Match against the Syntax->name strings defined in ved.c */
    if (strncmp(syntax_name, "C/C++",      5) == 0)  return "clangd";
    if (strncmp(syntax_name, "Python",     6) == 0)  return "pylsp";
    if (strncmp(syntax_name, "JavaScript", 10) == 0) return "typescript-language-server --stdio";
    if (strncmp(syntax_name, "Shell",      5) == 0)  return "bash-language-server start";
    if (strncmp(syntax_name, "Rust",       4) == 0)  return "rust-analyzer";
    return NULL;
}
