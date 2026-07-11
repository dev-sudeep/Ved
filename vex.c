/* ============================================================================
 * vex.c  --  Visual Editor eXperience
 * A single-file, dependency-light (libc + libutil) terminal IDE inspired by
 * Visual Studio Code: file explorer, tabbed editor with syntax highlighting,
 * integrated ANSI terminal (real PTY + shell), command palette, and DOS-Edit
 * style modal dialogs. Dark/Light themes approximate VS Code Dark+/Light+.
 *
 * Build:   gcc -O2 -Wall -o vex vex.c -lutil
 * Run:     ./vex [file-or-directory]
 *
 * Keys:
 *   Ctrl-E  toggle file explorer         Ctrl-T  toggle integrated terminal
 *   Ctrl-P  command palette              Ctrl-S  save
 *   Ctrl-N  new file                     Ctrl-W  close tab
 *   Ctrl-Q  quit                         Ctrl-\  cycle focus between panes
 *   Ctrl-Right/Left  next/prev tab       Ctrl-K Ctrl-T  toggle theme
 *   Mouse click: explorer entries open/expand, tabs switch, panes get focus.
 * ==========================================================================*/
#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <ctype.h>
#include <limits.h>
#include <pty.h>
#include <stdarg.h>

/* =========================== constants & enums =========================== */
#define VEX_VERSION "0.1"
#define TAB_STOP 4
#define MAX_TABS 32
#define STATUS_MSG_SECS 4
#define EXPLORER_MIN_W 18
#define TERM_MIN_H 5

enum Theme { THEME_DARK = 0, THEME_LIGHT = 1 };

enum Focus { FOCUS_EDITOR = 0, FOCUS_EXPLORER, FOCUS_TERMINAL };

enum Hl {
    HL_NORMAL = 0,
    HL_COMMENT,
    HL_KEYWORD1,   /* control-flow / language keywords */
    HL_KEYWORD2,   /* types */
    HL_STRING,
    HL_NUMBER,
    HL_FUNCTION,
    HL_OPERATOR,
    HL_BRACKET0,
    HL_BRACKET1,
    HL_BRACKET2,
    HL_VARIABLE,
    HL_COUNT
};

#define HL_FLAG_NUMBERS   (1 << 0)
#define HL_FLAG_STRINGS   (1 << 1)

enum EditorKey {
    KEY_BACKSPACE = 127,
    KEY_ARROW_LEFT = 1000,
    KEY_ARROW_RIGHT,
    KEY_ARROW_UP,
    KEY_ARROW_DOWN,
    KEY_DEL,
    KEY_HOME,
    KEY_END,
    KEY_PAGE_UP,
    KEY_PAGE_DOWN,
    KEY_CTRL_LEFT,
    KEY_CTRL_RIGHT,
    KEY_MOUSE,
    KEY_ESCAPE_ALONE
};

enum DialogType { DLG_NONE = 0, DLG_CONFIRM, DLG_INPUT, DLG_MESSAGE };

enum PendingAction {
    ACT_NONE = 0,
    ACT_CLOSE_TAB_SAVEPROMPT,
    ACT_QUIT_SAVEPROMPT,
    ACT_SAVE_AS_INPUT,
    ACT_RENAME_UNNAMED_ON_SAVE,
    ACT_NEW_FILE_INPUT
};

/* =============================== data types =============================== */

typedef struct {
    int size, rsize;
    char *chars;
    char *render;
    unsigned char *hl;
    int hl_open_comment;
} erow;

typedef struct Syntax {
    char *name;
    char **filematch;
    char **keywords1;
    char **keywords2;
    char *sl_comment;
    char *ml_comment_start;
    char *ml_comment_end;
    int flags;
} Syntax;

typedef struct {
    char *filename;      /* NULL => unnamed */
    int numrows;
    int rowcap;
    erow *row;
    int cx, cy, rx;
    int rowoff, coloff;
    int dirty;
    Syntax *syntax;
} EBuffer;

typedef struct DirNode {
    char name[256];
    char path[PATH_MAX];
    int is_dir;
    int expanded;
    int scanned;
    struct DirNode *children;
    int nchildren;
    int depth;
} DirNode;

typedef struct {
    unsigned char ch;
    unsigned char fg_r, fg_g, fg_b;
    unsigned char bg_r, bg_g, bg_b;
    unsigned char bold;
    unsigned char use_def_fg, use_def_bg;
} TCell;

typedef struct {
    int rows, cols;
    TCell *grid;
    int cx, cy;
    int scroll_top, scroll_bot;
    unsigned char cur_fg_r, cur_fg_g, cur_fg_b;
    unsigned char cur_bg_r, cur_bg_g, cur_bg_b;
    int cur_bold, cur_def_fg, cur_def_bg;
    int in_esc;      /* 0 none, 1 saw ESC, 2 in CSI */
    char esc_buf[128];
    int esc_len;
    int master_fd;
    pid_t child_pid;
    int alive;
} TermEmu;

typedef struct {
    enum DialogType type;
    char title[128];
    char message[512];
    char input[512];
    int input_len;
    int selected;
    int nbuttons;
    char *buttons[3];
    enum PendingAction action;
    int ctx_int;
} Dialog;

typedef struct {
    int screenrows, screencols;
    int show_explorer, show_terminal;
    int explorer_width, terminal_height;
    int theme;
    int focus;
    int focus_before_palette;
    struct termios orig_termios;
    int raw_enabled;

    EBuffer tabs[MAX_TABS];
    int ntabs, curtab;

    DirNode root;
    int have_root;
    DirNode **flat;
    int nflat, flatcap;
    int explorer_scroll;
    int explorer_sel;

    TermEmu term;

    Dialog dlg;

    int palette_active;
    char palette_query[256];
    int palette_len;
    int palette_sel;

    char statusmsg[256];
    time_t statusmsg_time;

    volatile sig_atomic_t resized;
    volatile sig_atomic_t sigchld_flag;
    int quit;

    /* layout geometry, recomputed each frame */
    int topbar_h;
    int explorer_x, explorer_y, explorer_h;
    int editor_x, editor_y, editor_w, editor_h;
    int term_x, term_y, term_w, term_h;
} State;

static State S;

/* ============================ forward decls ============================ */
static void dieMsg(const char *s);
static void refreshScreen(void);
static void setStatus(const char *fmt, ...);
static EBuffer *curBuf(void);
static void editorOpen(const char *filename, int newTab);
static void editorSave(EBuffer *b);
static void closeDialog(void);
static void openConfirmDialog(const char *title, const char *msg, char *b1, char *b2, char *b3, enum PendingAction act, int ctx);
static void openInputDialog(const char *title, const char *msg, const char *def, enum PendingAction act, int ctx);
static void openMessageDialog(const char *title, const char *msg);
static void explorerRebuildFlat(void);
static void termWrite(const char *buf, size_t n);
static void executeCommand(const char *cmd);
static void closeTab(int idx);

/* ============================ theme color tables ========================= */
typedef struct { unsigned char r,g,b; } RGB;

static RGB THEME_FG[2][HL_COUNT];
static RGB THEME_BG[2];
static RGB THEME_EDITOR_FG[2];
static RGB THEME_LINE_NUM[2];
static RGB THEME_LINE_NUM_ACTIVE[2];
static RGB THEME_STATUSBAR_BG[2];
static RGB THEME_STATUSBAR_FG[2];
static RGB THEME_TOPBAR_BG[2];
static RGB THEME_TAB_ACTIVE_BG[2];
static RGB THEME_TAB_INACTIVE_BG[2];
static RGB THEME_BORDER[2];
static RGB THEME_SELECTION_BG[2];
static RGB THEME_EXPLORER_BG[2];
static RGB THEME_EXPLORER_SEL_BG[2];
static RGB THEME_DIALOG_BG[2];
static RGB THEME_DIALOG_BORDER[2];
static RGB THEME_ACCENT[2];

static void initThemes(void) {
    /* ---- Dark (VS Code Dark+) ---- */
    THEME_BG[THEME_DARK]              = (RGB){30,30,30};
    THEME_EDITOR_FG[THEME_DARK]       = (RGB){212,212,212};
    THEME_LINE_NUM[THEME_DARK]        = (RGB){133,133,133};
    THEME_LINE_NUM_ACTIVE[THEME_DARK] = (RGB){197,197,197};
    THEME_STATUSBAR_BG[THEME_DARK]    = (RGB){0,122,204};
    THEME_STATUSBAR_FG[THEME_DARK]    = (RGB){255,255,255};
    THEME_TOPBAR_BG[THEME_DARK]       = (RGB){51,51,51};
    THEME_TAB_ACTIVE_BG[THEME_DARK]   = (RGB){30,30,30};
    THEME_TAB_INACTIVE_BG[THEME_DARK] = (RGB){45,45,45};
    THEME_BORDER[THEME_DARK]          = (RGB){60,60,60};
    THEME_SELECTION_BG[THEME_DARK]    = (RGB){38,79,120};
    THEME_EXPLORER_BG[THEME_DARK]     = (RGB){37,37,38};
    THEME_EXPLORER_SEL_BG[THEME_DARK] = (RGB){55,79,111};
    THEME_DIALOG_BG[THEME_DARK]       = (RGB){45,45,48};
    THEME_DIALOG_BORDER[THEME_DARK]   = (RGB){0,122,204};
    THEME_ACCENT[THEME_DARK]          = (RGB){0,122,204};

    THEME_FG[THEME_DARK][HL_NORMAL]   = (RGB){212,212,212};
    THEME_FG[THEME_DARK][HL_COMMENT]  = (RGB){106,153,85};
    THEME_FG[THEME_DARK][HL_KEYWORD1] = (RGB){86,156,214};
    THEME_FG[THEME_DARK][HL_KEYWORD2] = (RGB){78,201,176};
    THEME_FG[THEME_DARK][HL_STRING]   = (RGB){206,145,120};
    THEME_FG[THEME_DARK][HL_NUMBER]   = (RGB){181,206,168};
    THEME_FG[THEME_DARK][HL_FUNCTION] = (RGB){220,220,170};
    THEME_FG[THEME_DARK][HL_OPERATOR] = (RGB){212,212,212};
    THEME_FG[THEME_DARK][HL_BRACKET0] = (RGB){255,215,0};
    THEME_FG[THEME_DARK][HL_BRACKET1] = (RGB){218,112,214};
    THEME_FG[THEME_DARK][HL_BRACKET2] = (RGB){23,148,196};
    THEME_FG[THEME_DARK][HL_VARIABLE] = (RGB){156,220,254};

    /* ---- Light (VS Code Light+) ---- */
    THEME_BG[THEME_LIGHT]              = (RGB){255,255,255};
    THEME_EDITOR_FG[THEME_LIGHT]       = (RGB){0,0,0};
    THEME_LINE_NUM[THEME_LIGHT]        = (RGB){237,237,237};
    THEME_LINE_NUM_ACTIVE[THEME_LIGHT] = (RGB){0,0,0};
    THEME_STATUSBAR_BG[THEME_LIGHT]    = (RGB){0,122,204};
    THEME_STATUSBAR_FG[THEME_LIGHT]    = (RGB){255,255,255};
    THEME_TOPBAR_BG[THEME_LIGHT]       = (RGB){236,236,236};
    THEME_TAB_ACTIVE_BG[THEME_LIGHT]   = (RGB){255,255,255};
    THEME_TAB_INACTIVE_BG[THEME_LIGHT] = (RGB){236,236,236};
    THEME_BORDER[THEME_LIGHT]          = (RGB){200,200,200};
    THEME_SELECTION_BG[THEME_LIGHT]    = (RGB){173,214,255};
    THEME_EXPLORER_BG[THEME_LIGHT]     = (RGB){245,245,245};
    THEME_EXPLORER_SEL_BG[THEME_LIGHT] = (RGB){200,224,247};
    THEME_DIALOG_BG[THEME_LIGHT]       = (RGB){240,240,240};
    THEME_DIALOG_BORDER[THEME_LIGHT]   = (RGB){0,122,204};
    THEME_ACCENT[THEME_LIGHT]          = (RGB){0,122,204};

    THEME_FG[THEME_LIGHT][HL_NORMAL]   = (RGB){0,0,0};
    THEME_FG[THEME_LIGHT][HL_COMMENT]  = (RGB){0,128,0};
    THEME_FG[THEME_LIGHT][HL_KEYWORD1] = (RGB){0,0,255};
    THEME_FG[THEME_LIGHT][HL_KEYWORD2] = (RGB){38,127,153};
    THEME_FG[THEME_LIGHT][HL_STRING]   = (RGB){163,21,21};
    THEME_FG[THEME_LIGHT][HL_NUMBER]   = (RGB){9,134,88};
    THEME_FG[THEME_LIGHT][HL_FUNCTION] = (RGB){121,94,38};
    THEME_FG[THEME_LIGHT][HL_OPERATOR] = (RGB){0,0,0};
    THEME_FG[THEME_LIGHT][HL_BRACKET0] = (RGB){4,49,250};
    THEME_FG[THEME_LIGHT][HL_BRACKET1] = (RGB){49,131,49};
    THEME_FG[THEME_LIGHT][HL_BRACKET2] = (RGB){123,56,20};
    THEME_FG[THEME_LIGHT][HL_VARIABLE] = (RGB){0,16,128};
}

/* =============================== append buffer =========================== */
typedef struct { char *b; int len, cap; } abuf;
static void abInit(abuf *ab) { ab->b = NULL; ab->len = 0; ab->cap = 0; }
static void abAppend(abuf *ab, const char *s, int len) {
    if (ab->len + len > ab->cap) {
        int newcap = ab->cap ? ab->cap * 2 : 1024;
        while (newcap < ab->len + len) newcap *= 2;
        char *n = realloc(ab->b, newcap);
        if (!n) return;
        ab->b = n; ab->cap = newcap;
    }
    memcpy(ab->b + ab->len, s, len);
    ab->len += len;
}
static void abAppendStr(abuf *ab, const char *s) { abAppend(ab, s, (int)strlen(s)); }
static void abAppendFmt(abuf *ab, const char *fmt, ...) {
    char tmp[512];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n > 0) abAppend(ab, tmp, n < (int)sizeof(tmp) ? n : (int)sizeof(tmp) - 1);
}
static void abFree(abuf *ab) { free(ab->b); ab->b = NULL; ab->len = ab->cap = 0; }

static void abFg(abuf *ab, RGB c) { abAppendFmt(ab, "\x1b[38;2;%d;%d;%dm", c.r, c.g, c.b); }
static void abBg(abuf *ab, RGB c) { abAppendFmt(ab, "\x1b[48;2;%d;%d;%dm", c.r, c.g, c.b); }
static void abReset(abuf *ab) { abAppendStr(ab, "\x1b[0m"); }
static void abGoto(abuf *ab, int row, int col) { abAppendFmt(ab, "\x1b[%d;%dH", row + 1, col + 1); }

/* =============================== terminal raw mode ======================== */
static void disableRawMode(void) {
    if (S.raw_enabled) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &S.orig_termios);
        /* disable mouse tracking, restore cursor, main screen buffer */
        printf("\x1b[?1006l\x1b[?1000l\x1b[?25h\x1b[?1049l");
        fflush(stdout);
        S.raw_enabled = 0;
    }
}

static void dieMsg(const char *s) {
    disableRawMode();
    fprintf(stderr, "vex: %s: %s\n", s, strerror(errno));
    exit(1);
}

static void enableRawMode(void) {
    if (tcgetattr(STDIN_FILENO, &S.orig_termios) == -1) dieMsg("tcgetattr");
    struct termios raw = S.orig_termios;
    raw.c_iflag &= ~(unsigned long)(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(unsigned long)(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(unsigned long)(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) dieMsg("tcsetattr");
    S.raw_enabled = 1;
    /* alternate screen buffer, hide cursor, enable SGR mouse mode */
    printf("\x1b[?1049h\x1b[?1000h\x1b[?1006h");
    fflush(stdout);
}

static int getWindowSize(int *rows, int *cols) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) return -1;
    *cols = ws.ws_col; *rows = ws.ws_row;
    return 0;
}

static void handleSigwinch(int sig) { (void)sig; S.resized = 1; }
static void handleSigchld(int sig) { (void)sig; S.sigchld_flag = 1; }

/* =============================== small helpers ============================ */
static void *xmalloc(size_t n) { void *p = malloc(n); if (!p) dieMsg("malloc"); return p; }
static void *xrealloc(void *p, size_t n) { void *q = realloc(p, n); if (!q) dieMsg("realloc"); return q; }
static char *xstrdup(const char *s) { if (!s) return NULL; char *d = xmalloc(strlen(s) + 1); strcpy(d, s); return d; }


/* ============================== syntax tables ============================= */
static char *C_filematch[] = {".c", ".h", ".cpp", ".hpp", ".cc", ".cxx", NULL};
static char *C_kw1[] = {
    "if","else","for","while","do","switch","case","default","break","continue",
    "return","goto","struct","union","enum","typedef","sizeof","static","extern",
    "const","volatile","register","inline","void","auto","namespace","class",
    "public","private","protected","virtual","new","delete","try","catch","throw",
    "template","using","this","operator","friend","explicit","override", NULL
};
static char *C_kw2[] = {
    "int","long","short","char","float","double","unsigned","signed","bool",
    "size_t","uint8_t","uint16_t","uint32_t","uint64_t","int8_t","int16_t",
    "int32_t","int64_t","FILE","pid_t","ssize_t","va_list", NULL
};

static char *PY_filematch[] = {".py", ".pyw", NULL};
static char *PY_kw1[] = {
    "if","elif","else","for","while","def","class","return","yield","import",
    "from","as","with","try","except","finally","raise","break","continue",
    "pass","lambda","global","nonlocal","assert","del","in","is","not","and",
    "or","async","await", NULL
};
static char *PY_kw2[] = {
    "int","float","str","bool","list","dict","tuple","set","bytes","None",
    "True","False","object","type","self","cls", NULL
};

static char *JS_filematch[] = {".js", ".jsx", ".ts", ".tsx", ".mjs", NULL};
static char *JS_kw1[] = {
    "if","else","for","while","do","switch","case","default","break","continue",
    "return","function","var","let","const","class","extends","new","delete",
    "typeof","instanceof","in","of","try","catch","finally","throw","yield",
    "async","await","import","export","from","as","this","super","static","get","set", NULL
};
static char *JS_kw2[] = {
    "number","string","boolean","any","void","undefined","null","object",
    "true","false","Array","Object","String","Number","Boolean","Promise", NULL
};

static char *SH_filematch[] = {".sh", ".bash", NULL};
static char *SH_kw1[] = {
    "if","then","else","elif","fi","for","while","do","done","case","esac",
    "function","return","break","continue","in","local","export","readonly", NULL
};
static char *SH_kw2[] = { "true","false", NULL };

static char *RS_filematch[] = {".rs", NULL};
static char *RS_kw1[] = {
    "fn","let","mut","if","else","match","for","while","loop","return","break",
    "continue","struct","enum","impl","trait","pub","use","mod","crate","self",
    "Self","as","in","where","move","ref","static","const","unsafe","async","await", NULL
};
static char *RS_kw2[] = {
    "i8","i16","i32","i64","u8","u16","u32","u64","f32","f64","bool","char",
    "str","String","Vec","Option","Result","Box","true","false", NULL
};

static Syntax HLDB[] = {
    { "C/C++", C_filematch, C_kw1, C_kw2, "//", "/*", "*/", HL_FLAG_NUMBERS | HL_FLAG_STRINGS },
    { "Python", PY_filematch, PY_kw1, PY_kw2, "#", "\"\"\"", "\"\"\"", HL_FLAG_NUMBERS | HL_FLAG_STRINGS },
    { "JavaScript/TS", JS_filematch, JS_kw1, JS_kw2, "//", "/*", "*/", HL_FLAG_NUMBERS | HL_FLAG_STRINGS },
    { "Shell", SH_filematch, SH_kw1, SH_kw2, "#", NULL, NULL, HL_FLAG_NUMBERS | HL_FLAG_STRINGS },
    { "Rust", RS_filematch, RS_kw1, RS_kw2, "//", "/*", "*/", HL_FLAG_NUMBERS | HL_FLAG_STRINGS },
};
#define HLDB_ENTRIES (sizeof(HLDB) / sizeof(HLDB[0]))

static int isSeparator(int c) {
    return isspace(c) || c == '\0' || strchr(",.()+-/*=~%<>[]{}:;\"'!&|^?", c) != NULL;
}

static Syntax *syntaxForFilename(const char *filename) {
    if (!filename) return NULL;
    const char *ext = strrchr(filename, '.');
    for (size_t j = 0; j < HLDB_ENTRIES; j++) {
        Syntax *s = &HLDB[j];
        for (int i = 0; s->filematch[i]; i++) {
            int patlen = (int)strlen(s->filematch[i]);
            int is_ext = s->filematch[i][0] == '.';
            if (is_ext && ext && !strcmp(ext, s->filematch[i])) return s;
            if (!is_ext && strstr(filename, s->filematch[i])) return s;
            (void)patlen;
        }
    }
    return NULL;
}


/* ================================ row engine =============================== */

static int isKeywordIn(char **list, const char *word, int len) {
    for (int i = 0; list[i]; i++) {
        int klen = (int)strlen(list[i]);
        if (klen == len && !strncmp(list[i], word, len)) return 1;
    }
    return 0;
}

static void updateRender(erow *row) {
    int tabs = 0;
    for (int j = 0; j < row->size; j++) if (row->chars[j] == '\t') tabs++;
    free(row->render);
    row->render = xmalloc((size_t)row->size + (size_t)tabs * (TAB_STOP - 1) + 1);
    int idx = 0;
    for (int j = 0; j < row->size; j++) {
        if (row->chars[j] == '\t') {
            row->render[idx++] = ' ';
            while (idx % TAB_STOP != 0) row->render[idx++] = ' ';
        } else {
            row->render[idx++] = row->chars[j];
        }
    }
    row->render[idx] = '\0';
    row->rsize = idx;
}

/* Compute syntax highlight for one row. Returns 1 if hl_open_comment changed
 * (so caller may need to propagate to following rows). */
static int updateSyntaxRow(EBuffer *b, int rowIdx, int bracketDepthIn) {
    erow *row = &b->row[rowIdx];
    free(row->hl);
    row->hl = xmalloc((size_t)row->rsize + 1);
    memset(row->hl, HL_NORMAL, (size_t)row->rsize);

    Syntax *syn = b->syntax;
    if (!syn) { row->hl_open_comment = 0; return 0; }

    char *scs = syn->sl_comment;
    char *mcs = syn->ml_comment_start;
    char *mce = syn->ml_comment_end;
    int scs_len = scs ? (int)strlen(scs) : 0;
    int mcs_len = mcs ? (int)strlen(mcs) : 0;
    int mce_len = mce ? (int)strlen(mce) : 0;

    int prev_sep = 1;
    int in_string = 0;
    char string_quote = 0;
    int in_comment = (rowIdx > 0 && b->row[rowIdx - 1].hl_open_comment);
    int depth = bracketDepthIn;

    int i = 0;
    while (i < row->rsize) {
        char c = row->render[i];
        unsigned char prev_hl = (i > 0) ? row->hl[i - 1] : HL_NORMAL;

        if (scs_len && !in_string && !in_comment) {
            if (!strncmp(&row->render[i], scs, (size_t)scs_len)) {
                memset(&row->hl[i], HL_COMMENT, (size_t)(row->rsize - i));
                break;
            }
        }

        if (mcs_len && mce_len && !in_string) {
            if (in_comment) {
                row->hl[i] = HL_COMMENT;
                if (!strncmp(&row->render[i], mce, (size_t)mce_len)) {
                    memset(&row->hl[i], HL_COMMENT, (size_t)mce_len);
                    i += mce_len;
                    in_comment = 0;
                    prev_sep = 1;
                    continue;
                }
                i++;
                continue;
            } else if (!strncmp(&row->render[i], mcs, (size_t)mcs_len)) {
                memset(&row->hl[i], HL_COMMENT, (size_t)mcs_len);
                i += mcs_len;
                in_comment = 1;
                continue;
            }
        }

        if (syn->flags & HL_FLAG_STRINGS) {
            if (in_string) {
                row->hl[i] = HL_STRING;
                if (c == '\\' && i + 1 < row->rsize) {
                    row->hl[i + 1] = HL_STRING;
                    i += 2;
                    continue;
                }
                if (c == string_quote) in_string = 0;
                i++;
                prev_sep = 1;
                continue;
            } else if (c == '"' || c == '\'' || c == '`') {
                in_string = 1;
                string_quote = c;
                row->hl[i] = HL_STRING;
                i++;
                continue;
            }
        }

        if (syn->flags & HL_FLAG_NUMBERS) {
            if ((isdigit((unsigned char)c) && (prev_sep || prev_hl == HL_NUMBER)) ||
                (c == '.' && prev_hl == HL_NUMBER)) {
                row->hl[i] = HL_NUMBER;
                i++;
                prev_sep = 0;
                continue;
            }
        }

        if (c == '(' || c == '[' || c == '{') {
            int lvl = depth % 3;
            row->hl[i] = (lvl == 0) ? HL_BRACKET0 : (lvl == 1) ? HL_BRACKET1 : HL_BRACKET2;
            depth++;
            i++;
            prev_sep = 1;
            continue;
        }
        if (c == ')' || c == ']' || c == '}') {
            if (depth > 0) depth--;
            int lvl = depth % 3;
            row->hl[i] = (lvl == 0) ? HL_BRACKET0 : (lvl == 1) ? HL_BRACKET1 : HL_BRACKET2;
            i++;
            prev_sep = 1;
            continue;
        }

        if (strchr("+-*/%=<>!&|^~", c)) {
            row->hl[i] = HL_OPERATOR;
            i++;
            prev_sep = 1;
            continue;
        }

        if (prev_sep) {
            int j = i;
            while (j < row->rsize && (isalnum((unsigned char)row->render[j]) || row->render[j] == '_')) j++;
            int wlen = j - i;
            if (wlen > 0) {
                if (syn->keywords1 && isKeywordIn(syn->keywords1, &row->render[i], wlen)) {
                    memset(&row->hl[i], HL_KEYWORD1, (size_t)wlen);
                } else if (syn->keywords2 && isKeywordIn(syn->keywords2, &row->render[i], wlen)) {
                    memset(&row->hl[i], HL_KEYWORD2, (size_t)wlen);
                } else {
                    /* function-call detection: identifier followed by ( */
                    int k = j;
                    while (k < row->rsize && isspace((unsigned char)row->render[k])) k++;
                    if (k < row->rsize && row->render[k] == '(') {
                        memset(&row->hl[i], HL_FUNCTION, (size_t)wlen);
                    } else if (isupper((unsigned char)row->render[i]) == 0) {
                        memset(&row->hl[i], HL_VARIABLE, (size_t)wlen);
                    }
                }
                i = j;
                prev_sep = 0;
                continue;
            }
        }

        prev_sep = isSeparator((unsigned char)c);
        i++;
    }

    int changed = (row->hl_open_comment != in_comment);
    row->hl_open_comment = in_comment;
    return changed;
}

/* Full accurate rehighlight maintaining cumulative bracket depth and open-comment
 * state across the whole buffer. Called after edits; buffers are typically small
 * enough (source files) that this is fast. */
static void updateSyntaxAll(EBuffer *b) {
    int depth = 0;
    for (int r = 0; r < b->numrows; r++) {
        updateSyntaxRow(b, r, depth);
        erow *row = &b->row[r];
        for (int k = 0; k < row->rsize; k++) {
            char c = row->render[k];
            if (c == '(' || c == '[' || c == '{') depth++;
            else if (c == ')' || c == ']' || c == '}') { if (depth > 0) depth--; }
        }
    }
}


/* ============================== buffer editing ============================= */

static EBuffer *curBuf(void) {
    if (S.ntabs == 0) return NULL;
    return &S.tabs[S.curtab];
}

static void bufferInsertRow(EBuffer *b, int at, const char *s, size_t len) {
    if (at < 0 || at > b->numrows) return;
    if (b->numrows + 1 > b->rowcap) {
        b->rowcap = b->rowcap ? b->rowcap * 2 : 64;
        b->row = xrealloc(b->row, sizeof(erow) * (size_t)b->rowcap);
    }
    memmove(&b->row[at + 1], &b->row[at], sizeof(erow) * (size_t)(b->numrows - at));
    b->row[at].size = (int)len;
    b->row[at].chars = xmalloc(len + 1);
    memcpy(b->row[at].chars, s, len);
    b->row[at].chars[len] = '\0';
    b->row[at].render = NULL;
    b->row[at].rsize = 0;
    b->row[at].hl = NULL;
    b->row[at].hl_open_comment = 0;
    b->numrows++;
    updateRender(&b->row[at]);
}

static void freeRow(erow *row) {
    free(row->chars);
    free(row->render);
    free(row->hl);
}

static void bufferDelRow(EBuffer *b, int at) {
    if (at < 0 || at >= b->numrows) return;
    freeRow(&b->row[at]);
    memmove(&b->row[at], &b->row[at + 1], sizeof(erow) * (size_t)(b->numrows - at - 1));
    b->numrows--;
}

static void rowInsertChar(erow *row, int at, int c) {
    if (at < 0 || at > row->size) at = row->size;
    row->chars = xrealloc(row->chars, (size_t)row->size + 2);
    memmove(&row->chars[at + 1], &row->chars[at], (size_t)(row->size - at + 1));
    row->size++;
    row->chars[at] = (char)c;
    updateRender(row);
}

static void rowAppendString(erow *row, const char *s, size_t len) {
    row->chars = xrealloc(row->chars, (size_t)row->size + len + 1);
    memcpy(&row->chars[row->size], s, len);
    row->size += (int)len;
    row->chars[row->size] = '\0';
    updateRender(row);
}

static void rowDelChar(erow *row, int at) {
    if (at < 0 || at >= row->size) return;
    memmove(&row->chars[at], &row->chars[at + 1], (size_t)(row->size - at));
    row->size--;
    updateRender(row);
}

static void bufMarkDirty(EBuffer *b) { b->dirty = 1; }

static void editorInsertChar(EBuffer *b, int c) {
    if (b->cy == b->numrows) bufferInsertRow(b, b->numrows, "", 0);
    rowInsertChar(&b->row[b->cy], b->cx, c);
    b->cx++;
    bufMarkDirty(b);
    updateSyntaxAll(b);
}

static void editorInsertNewline(EBuffer *b) {
    if (b->cx == 0) {
        bufferInsertRow(b, b->cy, "", 0);
    } else {
        erow *row = &b->row[b->cy];
        bufferInsertRow(b, b->cy + 1, &row->chars[b->cx], (size_t)(row->size - b->cx));
        row = &b->row[b->cy];
        row->size = b->cx;
        row->chars[row->size] = '\0';
        updateRender(row);
    }
    b->cy++;
    b->cx = 0;
    bufMarkDirty(b);
    updateSyntaxAll(b);
}

static void editorDelChar(EBuffer *b) {
    if (b->cy == b->numrows) return;
    if (b->cx == 0 && b->cy == 0) return;
    erow *row = &b->row[b->cy];
    if (b->cx > 0) {
        rowDelChar(row, b->cx - 1);
        b->cx--;
    } else {
        int prevlen = b->row[b->cy - 1].size;
        rowAppendString(&b->row[b->cy - 1], row->chars, (size_t)row->size);
        bufferDelRow(b, b->cy);
        b->cy--;
        b->cx = prevlen;
    }
    bufMarkDirty(b);
    updateSyntaxAll(b);
}

static void editorDelForward(EBuffer *b) {
    if (b->cy == b->numrows) return;
    erow *row = &b->row[b->cy];
    if (b->cx < row->size) {
        rowDelChar(row, b->cx);
    } else if (b->cy < b->numrows - 1) {
        rowAppendString(row, b->row[b->cy + 1].chars, (size_t)b->row[b->cy + 1].size);
        bufferDelRow(b, b->cy + 1);
    } else return;
    bufMarkDirty(b);
    updateSyntaxAll(b);
}

static char *bufferToString(EBuffer *b, size_t *outlen) {
    size_t total = 0;
    for (int i = 0; i < b->numrows; i++) total += (size_t)b->row[i].size + 1;
    char *buf = xmalloc(total + 1);
    char *p = buf;
    for (int i = 0; i < b->numrows; i++) {
        memcpy(p, b->row[i].chars, (size_t)b->row[i].size);
        p += b->row[i].size;
        *p++ = '\n';
    }
    *outlen = total;
    return buf;
}

static void bufferFreeAll(EBuffer *b) {
    for (int i = 0; i < b->numrows; i++) freeRow(&b->row[i]);
    free(b->row);
    free(b->filename);
    memset(b, 0, sizeof(*b));
}

/* Initialize a fresh empty buffer struct (does not allocate a tab slot). */
static void bufferInit(EBuffer *b, const char *filename) {
    memset(b, 0, sizeof(*b));
    b->filename = filename ? xstrdup(filename) : NULL;
    b->syntax = syntaxForFilename(filename);
    bufferInsertRow(b, 0, "", 0);
}

static int newTab(const char *filename) {
    if (S.ntabs >= MAX_TABS) { setStatus("Too many tabs open"); return -1; }
    int idx = S.ntabs++;
    bufferInit(&S.tabs[idx], filename);
    S.curtab = idx;
    return idx;
}

static void loadFileIntoBuffer(EBuffer *b, const char *filename) {
    FILE *fp = fopen(filename, "r");
    if (!fp) { setStatus("Cannot open %s: %s", filename, strerror(errno)); return; }
    for (int i = 0; i < b->numrows; i++) freeRow(&b->row[i]);
    b->numrows = 0;
    char *line = NULL; size_t cap = 0; ssize_t len;
    while ((len = getline(&line, &cap, fp)) != -1) {
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) len--;
        bufferInsertRow(b, b->numrows, line, (size_t)len);
    }
    free(line);
    fclose(fp);
    if (b->numrows == 0) bufferInsertRow(b, 0, "", 0);
    b->dirty = 0;
    b->cx = b->cy = 0;
    free(b->filename);
    b->filename = xstrdup(filename);
    b->syntax = syntaxForFilename(filename);
    updateSyntaxAll(b);
}

/* Opens filename in a new tab, or focuses an already-open tab for that file. */
static void editorOpen(const char *filename, int forceNewTab) {
    (void)forceNewTab;
    char resolved[PATH_MAX];
    if (!realpath(filename, resolved)) strncpy(resolved, filename, sizeof(resolved) - 1);
    for (int i = 0; i < S.ntabs; i++) {
        if (S.tabs[i].filename) {
            char rp[PATH_MAX];
            if (realpath(S.tabs[i].filename, rp) && !strcmp(rp, resolved)) {
                S.curtab = i;
                S.focus = FOCUS_EDITOR;
                return;
            }
        }
    }
    struct stat st;
    if (stat(filename, &st) == -1) { setStatus("Cannot open %s", filename); return; }
    if (S_ISDIR(st.st_mode)) { setStatus("%s is a directory", filename); return; }
    int idx = newTab(filename);
    if (idx < 0) return;
    loadFileIntoBuffer(&S.tabs[idx], filename);
    S.focus = FOCUS_EDITOR;
}

static void editorSaveAs(EBuffer *b, const char *filename) {
    size_t len;
    char *data = bufferToString(b, &len);
    FILE *fp = fopen(filename, "w");
    if (!fp) { setStatus("Save failed: %s", strerror(errno)); free(data); return; }
    fwrite(data, 1, len, fp);
    fclose(fp);
    free(data);
    free(b->filename);
    b->filename = xstrdup(filename);
    b->syntax = syntaxForFilename(filename);
    updateSyntaxAll(b);
    b->dirty = 0;
    setStatus("Saved %s (%zu bytes)", filename, len);
    if (S.have_root) explorerRebuildFlat();
}

static void editorSave(EBuffer *b) {
    if (!b) return;
    if (!b->filename) {
        openInputDialog("Save As", "Enter filename to save this new file:", "", ACT_SAVE_AS_INPUT, S.curtab);
        return;
    }
    editorSaveAs(b, b->filename);
}


/* ============================== file explorer ============================== */

static int dirEntryCmp(const void *a, const void *b) {
    const DirNode *na = (const DirNode *)a, *nb = (const DirNode *)b;
    if (na->is_dir != nb->is_dir) return nb->is_dir - na->is_dir; /* dirs first */
    return strcasecmp(na->name, nb->name);
}

static void scanDir(DirNode *node) {
    if (node->scanned) return;
    node->scanned = 1;
    DIR *d = opendir(node->path);
    if (!d) return;
    struct dirent *ent;
    int cap = 16, n = 0;
    DirNode *kids = xmalloc(sizeof(DirNode) * (size_t)cap);
    while ((ent = readdir(d)) != NULL) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
        if (ent->d_name[0] == '.') continue; /* hide dotfiles, like VS Code default-ish */
        if (n >= cap) { cap *= 2; kids = xrealloc(kids, sizeof(DirNode) * (size_t)cap); }
        DirNode *kid = &kids[n++];
        memset(kid, 0, sizeof(*kid));
        strncpy(kid->name, ent->d_name, sizeof(kid->name) - 1);
        snprintf(kid->path, sizeof(kid->path), "%s/%s", node->path, ent->d_name);
        struct stat st;
        kid->is_dir = (stat(kid->path, &st) == 0) && S_ISDIR(st.st_mode);
        kid->depth = node->depth + 1;
    }
    closedir(d);
    qsort(kids, (size_t)n, sizeof(DirNode), dirEntryCmp);
    node->children = kids;
    node->nchildren = n;
}

static void freeDirNode(DirNode *node) {
    for (int i = 0; i < node->nchildren; i++) freeDirNode(&node->children[i]);
    free(node->children);
    node->children = NULL;
    node->nchildren = 0;
}

static void explorerSetRoot(const char *path) {
    if (S.have_root) freeDirNode(&S.root);
    memset(&S.root, 0, sizeof(S.root));
    char resolved[PATH_MAX];
    if (!realpath(path, resolved)) strncpy(resolved, path, sizeof(resolved) - 1);
    strncpy(S.root.path, resolved, sizeof(S.root.path) - 1);
    const char *base = strrchr(resolved, '/');
    strncpy(S.root.name, base ? base + 1 : resolved, sizeof(S.root.name) - 1);
    if (S.root.name[0] == '\0') strncpy(S.root.name, resolved, sizeof(S.root.name) - 1);
    S.root.is_dir = 1;
    S.root.expanded = 1;
    S.root.depth = 0;
    scanDir(&S.root);
    S.have_root = 1;
    explorerRebuildFlat();
}

static void flatPush(DirNode *n) {
    if (S.nflat >= S.flatcap) {
        S.flatcap = S.flatcap ? S.flatcap * 2 : 128;
        S.flat = xrealloc(S.flat, sizeof(DirNode *) * (size_t)S.flatcap);
    }
    S.flat[S.nflat++] = n;
}

static void flattenNode(DirNode *n) {
    flatPush(n);
    if (n->is_dir && n->expanded) {
        if (!n->scanned) scanDir(n);
        for (int i = 0; i < n->nchildren; i++) flattenNode(&n->children[i]);
    }
}

static void explorerRebuildFlat(void) {
    S.nflat = 0;
    if (!S.have_root) return;
    flattenNode(&S.root);
    if (S.explorer_sel >= S.nflat) S.explorer_sel = S.nflat > 0 ? S.nflat - 1 : 0;
}

static void explorerToggleOrOpen(int flatIdx) {
    if (flatIdx < 0 || flatIdx >= S.nflat) return;
    DirNode *n = S.flat[flatIdx];
    if (n->is_dir) {
        n->expanded = !n->expanded;
        if (n->expanded && !n->scanned) scanDir(n);
        explorerRebuildFlat();
    } else {
        editorOpen(n->path, 1);
    }
    S.explorer_sel = flatIdx;
}


/* ============================ embedded PTY terminal ========================= */

static void termResetCellDefaults(TermEmu *t) {
    t->cur_fg_r = t->cur_fg_g = t->cur_fg_b = 0;
    t->cur_bg_r = t->cur_bg_g = t->cur_bg_b = 0;
    t->cur_bold = 0;
    t->cur_def_fg = 1;
    t->cur_def_bg = 1;
}

static void termAllocGrid(TermEmu *t, int rows, int cols) {
    TCell *ng = xmalloc(sizeof(TCell) * (size_t)(rows * cols));
    for (int i = 0; i < rows * cols; i++) {
        ng[i].ch = ' ';
        ng[i].fg_r = ng[i].fg_g = ng[i].fg_b = 0;
        ng[i].bg_r = ng[i].bg_g = ng[i].bg_b = 0;
        ng[i].bold = 0;
        ng[i].use_def_fg = 1;
        ng[i].use_def_bg = 1;
    }
    if (t->grid) {
        int copyRows = rows < t->rows ? rows : t->rows;
        int copyCols = cols < t->cols ? cols : t->cols;
        for (int r = 0; r < copyRows; r++)
            for (int c = 0; c < copyCols; c++)
                ng[r * cols + c] = t->grid[r * t->cols + c];
        free(t->grid);
    }
    t->grid = ng;
    t->rows = rows;
    t->cols = cols;
    if (t->cy >= rows) t->cy = rows - 1;
    if (t->cx >= cols) t->cx = cols - 1;
    t->scroll_top = 0;
    t->scroll_bot = rows - 1;
}

static void termStartShell(TermEmu *t, int rows, int cols) {
    memset(t, 0, sizeof(*t));
    termResetCellDefaults(t);
    if (rows < 1) rows = 24;
    if (cols < 1) cols = 80;
    termAllocGrid(t, rows, cols);

    struct winsize ws;
    ws.ws_row = (unsigned short)rows;
    ws.ws_col = (unsigned short)cols;
    ws.ws_xpixel = ws.ws_ypixel = 0;

    pid_t pid = forkpty(&t->master_fd, NULL, NULL, &ws);
    if (pid == -1) { dieMsg("forkpty"); }
    if (pid == 0) {
        /* child */
        const char *shell = getenv("SHELL");
        if (!shell || !*shell) shell = "/bin/sh";
        setenv("TERM", "xterm-256color", 1);
        execl(shell, shell, "-i", (char *)NULL);
        _exit(127);
    }
    t->child_pid = pid;
    t->alive = 1;
    int fl = fcntl(t->master_fd, F_GETFL, 0);
    fcntl(t->master_fd, F_SETFL, fl | O_NONBLOCK);
}

static void termResize(TermEmu *t, int rows, int cols) {
    if (rows < 1 || cols < 1) return;
    if (rows == t->rows && cols == t->cols) return;
    termAllocGrid(t, rows, cols);
    struct winsize ws;
    ws.ws_row = (unsigned short)rows;
    ws.ws_col = (unsigned short)cols;
    ws.ws_xpixel = ws.ws_ypixel = 0;
    if (t->alive) ioctl(t->master_fd, TIOCSWINSZ, &ws);
}

static TCell *termCellAt(TermEmu *t, int r, int c) {
    if (r < 0) r = 0; if (r >= t->rows) r = t->rows - 1;
    if (c < 0) c = 0; if (c >= t->cols) c = t->cols - 1;
    return &t->grid[r * t->cols + c];
}

static void termScrollUp(TermEmu *t, int lines) {
    if (lines <= 0) return;
    int top = t->scroll_top, bot = t->scroll_bot;
    int region = bot - top + 1;
    if (lines > region) lines = region;
    for (int r = top; r <= bot - lines; r++)
        memcpy(&t->grid[r * t->cols], &t->grid[(r + lines) * t->cols], sizeof(TCell) * (size_t)t->cols);
    for (int r = bot - lines + 1; r <= bot; r++) {
        for (int c = 0; c < t->cols; c++) {
            TCell *cell = &t->grid[r * t->cols + c];
            cell->ch = ' '; cell->use_def_fg = 1; cell->use_def_bg = 1; cell->bold = 0;
        }
    }
}

static void termPutChar(TermEmu *t, unsigned char c) {
    if (c == '\r') { t->cx = 0; return; }
    if (c == '\n') {
        t->cy++;
        if (t->cy > t->scroll_bot) { termScrollUp(t, t->cy - t->scroll_bot); t->cy = t->scroll_bot; }
        return;
    }
    if (c == '\b') { if (t->cx > 0) t->cx--; return; }
    if (c == '\t') { t->cx = (t->cx / 8 + 1) * 8; if (t->cx >= t->cols) { t->cx = t->cols - 1; } return; }
    if (c == '\a') return; /* bell */
    if (c < 0x20) return;

    if (t->cx >= t->cols) { t->cx = 0; t->cy++; if (t->cy > t->scroll_bot) { termScrollUp(t, 1); t->cy = t->scroll_bot; } }
    TCell *cell = termCellAt(t, t->cy, t->cx);
    cell->ch = c;
    cell->fg_r = t->cur_fg_r; cell->fg_g = t->cur_fg_g; cell->fg_b = t->cur_fg_b;
    cell->bg_r = t->cur_bg_r; cell->bg_g = t->cur_bg_g; cell->bg_b = t->cur_bg_b;
    cell->bold = (unsigned char)t->cur_bold;
    cell->use_def_fg = (unsigned char)t->cur_def_fg;
    cell->use_def_bg = (unsigned char)t->cur_def_bg;
    t->cx++;
}

static RGB ansi16[16] = {
    {0,0,0},{205,49,49},{13,188,121},{229,229,16},{36,114,200},{188,63,188},{17,168,205},{229,229,229},
    {102,102,102},{241,76,76},{35,209,139},{245,245,67},{59,142,234},{214,112,214},{41,184,219},{255,255,255}
};

static void termApplySGR(TermEmu *t, int *params, int nparams) {
    if (nparams == 0) { termResetCellDefaults(t); return; }
    for (int i = 0; i < nparams; i++) {
        int p = params[i];
        if (p == 0) { termResetCellDefaults(t); }
        else if (p == 1) t->cur_bold = 1;
        else if (p == 22) t->cur_bold = 0;
        else if (p >= 30 && p <= 37) { RGB c = ansi16[p - 30]; t->cur_fg_r=c.r; t->cur_fg_g=c.g; t->cur_fg_b=c.b; t->cur_def_fg = 0; }
        else if (p == 39) { t->cur_def_fg = 1; }
        else if (p >= 90 && p <= 97) { RGB c = ansi16[8 + (p - 90)]; t->cur_fg_r=c.r; t->cur_fg_g=c.g; t->cur_fg_b=c.b; t->cur_def_fg = 0; }
        else if (p >= 40 && p <= 47) { RGB c = ansi16[p - 40]; t->cur_bg_r=c.r; t->cur_bg_g=c.g; t->cur_bg_b=c.b; t->cur_def_bg = 0; }
        else if (p == 49) { t->cur_def_bg = 1; }
        else if (p >= 100 && p <= 107) { RGB c = ansi16[8 + (p - 100)]; t->cur_bg_r=c.r; t->cur_bg_g=c.g; t->cur_bg_b=c.b; t->cur_def_bg = 0; }
        else if (p == 38 || p == 48) {
            int isFg = (p == 38);
            if (i + 1 < nparams && params[i + 1] == 5 && i + 2 < nparams) {
                int idx = params[i + 2];
                RGB c;
                if (idx < 16) c = ansi16[idx];
                else if (idx < 232) {
                    int idx2 = idx - 16;
                    int r = idx2 / 36, g = (idx2 / 6) % 6, bl = idx2 % 6;
                    c.r = (unsigned char)(r ? r * 40 + 55 : 0);
                    c.g = (unsigned char)(g ? g * 40 + 55 : 0);
                    c.b = (unsigned char)(bl ? bl * 40 + 55 : 0);
                } else {
                    int v = 8 + (idx - 232) * 10;
                    c.r = c.g = c.b = (unsigned char)v;
                }
                if (isFg) { t->cur_fg_r=c.r; t->cur_fg_g=c.g; t->cur_fg_b=c.b; t->cur_def_fg=0; }
                else { t->cur_bg_r=c.r; t->cur_bg_g=c.g; t->cur_bg_b=c.b; t->cur_def_bg=0; }
                i += 2;
            } else if (i + 1 < nparams && params[i + 1] == 2 && i + 4 < nparams) {
                unsigned char r = (unsigned char)params[i + 2], g = (unsigned char)params[i + 3], bl = (unsigned char)params[i + 4];
                if (isFg) { t->cur_fg_r=r; t->cur_fg_g=g; t->cur_fg_b=bl; t->cur_def_fg=0; }
                else { t->cur_bg_r=r; t->cur_bg_g=g; t->cur_bg_b=bl; t->cur_def_bg=0; }
                i += 4;
            }
        }
    }
}

static void termClearRegion(TermEmu *t, int r0, int c0, int r1, int c1) {
    for (int r = r0; r <= r1 && r < t->rows; r++) {
        int cs = (r == r0) ? c0 : 0;
        int ce = (r == r1) ? c1 : t->cols - 1;
        for (int c = cs; c <= ce && c < t->cols; c++) {
            TCell *cell = &t->grid[r * t->cols + c];
            cell->ch = ' '; cell->use_def_fg = 1; cell->use_def_bg = 1; cell->bold = 0;
        }
    }
}

static void termHandleCSI(TermEmu *t, const char *seq) {
    /* seq excludes leading ESC[ and trailing final byte; last char in esc_buf is final */
    int len = (int)strlen(seq);
    if (len == 0) return;
    char final = seq[len - 1];
    char params[64]; int plen = len - 1;
    if (plen < 0) plen = 0;
    if (plen > (int)sizeof(params) - 1) plen = sizeof(params) - 1;
    memcpy(params, seq, (size_t)plen);
    params[plen] = '\0';
    int priv = (plen > 0 && params[0] == '?');
    char *pp = priv ? params + 1 : params;

    int nums[32]; int nn = 0;
    char *tok = pp; char *save = NULL;
    if (*pp) {
        char *cursor = pp;
        char numbuf[16]; int nbi = 0;
        while (1) {
            char ch = *cursor;
            if (ch == ';' || ch == '\0') {
                numbuf[nbi] = '\0';
                nums[nn++] = nbi > 0 ? atoi(numbuf) : (final=='m'?0:-1);
                nbi = 0;
                if (ch == '\0') break;
            } else if (nbi < (int)sizeof(numbuf) - 1) {
                numbuf[nbi++] = ch;
            }
            if (nn >= 30) break;
            cursor++;
        }
    }
    (void)tok; (void)save;

    switch (final) {
        case 'H': case 'f': {
            int row = (nn > 0 && nums[0] > 0) ? nums[0] - 1 : 0;
            int col = (nn > 1 && nums[1] > 0) ? nums[1] - 1 : 0;
            t->cy = row; t->cx = col;
            if (t->cy >= t->rows) t->cy = t->rows - 1;
            if (t->cx >= t->cols) t->cx = t->cols - 1;
            break;
        }
        case 'A': t->cy -= (nn > 0 && nums[0] > 0) ? nums[0] : 1; if (t->cy < 0) t->cy = 0; break;
        case 'B': t->cy += (nn > 0 && nums[0] > 0) ? nums[0] : 1; if (t->cy >= t->rows) t->cy = t->rows - 1; break;
        case 'C': t->cx += (nn > 0 && nums[0] > 0) ? nums[0] : 1; if (t->cx >= t->cols) t->cx = t->cols - 1; break;
        case 'D': t->cx -= (nn > 0 && nums[0] > 0) ? nums[0] : 1; if (t->cx < 0) t->cx = 0; break;
        case 'G': t->cx = ((nn > 0 && nums[0] > 0) ? nums[0] - 1 : 0); if (t->cx >= t->cols) t->cx = t->cols-1; if (t->cx<0) t->cx=0; break;
        case 'd': t->cy = ((nn > 0 && nums[0] > 0) ? nums[0] - 1 : 0); if (t->cy >= t->rows) t->cy = t->rows-1; if (t->cy<0) t->cy=0; break;
        case 'J': {
            int mode = (nn > 0) ? nums[0] : 0;
            if (mode == 2 || mode == 3) termClearRegion(t, 0, 0, t->rows - 1, t->cols - 1);
            else if (mode == 1) termClearRegion(t, 0, 0, t->cy, t->cols - 1);
            else termClearRegion(t, t->cy, t->cx, t->rows - 1, t->cols - 1);
            break;
        }
        case 'K': {
            int mode = (nn > 0) ? nums[0] : 0;
            if (mode == 2) termClearRegion(t, t->cy, 0, t->cy, t->cols - 1);
            else if (mode == 1) termClearRegion(t, t->cy, 0, t->cy, t->cx);
            else termClearRegion(t, t->cy, t->cx, t->cy, t->cols - 1);
            break;
        }
        case 'm': termApplySGR(t, nums, nn > 0 ? nn : 0); break;
        case 'r': {
            int top = (nn > 0 && nums[0] > 0) ? nums[0] - 1 : 0;
            int bot = (nn > 1 && nums[1] > 0) ? nums[1] - 1 : t->rows - 1;
            if (top < 0) top = 0; if (bot >= t->rows) bot = t->rows - 1;
            if (top < bot) { t->scroll_top = top; t->scroll_bot = bot; }
            break;
        }
        case 'h': case 'l': /* mode set/reset (bracketed paste, cursor visibility, alt screen) - ignored */
            break;
        case 'S': termScrollUp(t, (nn > 0 && nums[0] > 0) ? nums[0] : 1); break;
        default: break;
    }
}

static void termFeed(TermEmu *t, const unsigned char *buf, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char c = buf[i];
        if (t->in_esc == 0) {
            if (c == 0x1b) { t->in_esc = 1; t->esc_len = 0; }
            else termPutChar(t, c);
        } else if (t->in_esc == 1) {
            if (c == '[') { t->in_esc = 2; t->esc_len = 0; }
            else if (c == ']') { t->in_esc = 3; t->esc_len = 0; } /* OSC, skip until BEL/ST */
            else { t->in_esc = 0; } /* single-char escapes ignored (e.g. ESC(B) */
        } else if (t->in_esc == 2) {
            if (t->esc_len < (int)sizeof(t->esc_buf) - 1) t->esc_buf[t->esc_len++] = (char)c;
            if ((c >= '@' && c <= '~')) {
                t->esc_buf[t->esc_len] = '\0';
                termHandleCSI(t, t->esc_buf);
                t->in_esc = 0;
            }
        } else if (t->in_esc == 3) {
            if (c == 0x07) t->in_esc = 0;
            else if (c == 0x1b) { /* possibly ST (ESC \\) */ }
        }
    }
}

static void termWrite(const char *buf, size_t n) {
    if (S.term.alive) {
        ssize_t w = write(S.term.master_fd, buf, n);
        (void)w;
    }
}


/* ================================== status ================================= */
static void setStatus(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vsnprintf(S.statusmsg, sizeof(S.statusmsg), fmt, ap);
    va_end(ap);
    S.statusmsg_time = time(NULL);
}

/* ================================== dialogs ================================= */
static void closeDialog(void) {
    S.dlg.type = DLG_NONE;
    S.dlg.action = ACT_NONE;
}

static void openConfirmDialog(const char *title, const char *msg, char *b1, char *b2, char *b3,
                               enum PendingAction act, int ctx) {
    memset(&S.dlg, 0, sizeof(S.dlg));
    S.dlg.type = DLG_CONFIRM;
    strncpy(S.dlg.title, title, sizeof(S.dlg.title) - 1);
    strncpy(S.dlg.message, msg, sizeof(S.dlg.message) - 1);
    S.dlg.buttons[0] = b1; S.dlg.buttons[1] = b2; S.dlg.buttons[2] = b3;
    S.dlg.nbuttons = b3 ? 3 : (b2 ? 2 : 1);
    S.dlg.selected = 0;
    S.dlg.action = act;
    S.dlg.ctx_int = ctx;
}

static void openInputDialog(const char *title, const char *msg, const char *def,
                             enum PendingAction act, int ctx) {
    memset(&S.dlg, 0, sizeof(S.dlg));
    S.dlg.type = DLG_INPUT;
    strncpy(S.dlg.title, title, sizeof(S.dlg.title) - 1);
    strncpy(S.dlg.message, msg, sizeof(S.dlg.message) - 1);
    if (def) { strncpy(S.dlg.input, def, sizeof(S.dlg.input) - 1); S.dlg.input_len = (int)strlen(S.dlg.input); }
    S.dlg.action = act;
    S.dlg.ctx_int = ctx;
}

static void openMessageDialog(const char *title, const char *msg) {
    memset(&S.dlg, 0, sizeof(S.dlg));
    S.dlg.type = DLG_MESSAGE;
    strncpy(S.dlg.title, title, sizeof(S.dlg.title) - 1);
    strncpy(S.dlg.message, msg, sizeof(S.dlg.message) - 1);
    S.dlg.buttons[0] = "OK";
    S.dlg.nbuttons = 1;
    S.dlg.action = ACT_NONE;
}

static void closeTabForceIndex(int idx) {
    bufferFreeAll(&S.tabs[idx]);
    for (int i = idx; i < S.ntabs - 1; i++) S.tabs[i] = S.tabs[i + 1];
    S.ntabs--;
    if (S.ntabs == 0) { S.curtab = 0; }
    else if (S.curtab >= S.ntabs) S.curtab = S.ntabs - 1;
    else if (S.curtab > idx) S.curtab--;
}

static void closeTab(int idx) {
    if (idx < 0 || idx >= S.ntabs) return;
    EBuffer *b = &S.tabs[idx];
    if (b->dirty) {
        S.curtab = idx;
        char msg[300];
        snprintf(msg, sizeof(msg), "\"%s\" has unsaved changes. Save before closing?",
                 b->filename ? b->filename : "Untitled");
        openConfirmDialog("Close File", msg, "Save", "Don't Save", "Cancel",
                           ACT_CLOSE_TAB_SAVEPROMPT, idx);
        return;
    }
    closeTabForceIndex(idx);
}

static void requestQuit(void) {
    for (int i = 0; i < S.ntabs; i++) {
        if (S.tabs[i].dirty) {
            char msg[300];
            snprintf(msg, sizeof(msg), "\"%s\" has unsaved changes. Save before quitting?",
                      S.tabs[i].filename ? S.tabs[i].filename : "Untitled");
            S.curtab = i;
            openConfirmDialog("Quit vex", msg, "Save", "Don't Save", "Cancel",
                               ACT_QUIT_SAVEPROMPT, i);
            return;
        }
    }
    S.quit = 1;
}

/* Called when a confirm dialog button is activated. */
static void dialogConfirm(int buttonIdx) {
    Dialog d = S.dlg;
    closeDialog();
    const char *label = (buttonIdx >= 0 && buttonIdx < d.nbuttons) ? d.buttons[buttonIdx] : "Cancel";
    int isSave = label && !strcmp(label, "Save");
    int isDontSave = label && !strcmp(label, "Don't Save");
    int isCancel = !isSave && !isDontSave;

    switch (d.action) {
        case ACT_CLOSE_TAB_SAVEPROMPT: {
            if (isCancel) return;
            if (isSave) {
                EBuffer *b = &S.tabs[d.ctx_int];
                if (!b->filename) {
                    openInputDialog("Save As", "Enter filename to save this new file:", "",
                                     ACT_RENAME_UNNAMED_ON_SAVE, d.ctx_int);
                    /* after save via input dialog we still need to close; store a flag by
                     * reusing ctx: we chain to close after save completes (handled below) */
                    return;
                }
                editorSaveAs(b, b->filename);
            }
            closeTabForceIndex(d.ctx_int);
            return;
        }
        case ACT_QUIT_SAVEPROMPT: {
            if (isCancel) return;
            if (isSave) {
                EBuffer *b = &S.tabs[d.ctx_int];
                if (!b->filename) {
                    openInputDialog("Save As", "Enter filename to save this new file:", "",
                                     ACT_SAVE_AS_INPUT, d.ctx_int);
                    return;
                }
                editorSaveAs(b, b->filename);
            }
            for (int i = 0; i < S.ntabs; i++) {
                if (S.tabs[i].dirty) {
                    char msg[300];
                    snprintf(msg, sizeof(msg), "\"%s\" has unsaved changes. Save before quitting?",
                              S.tabs[i].filename ? S.tabs[i].filename : "Untitled");
                    S.curtab = i;
                    openConfirmDialog("Quit vex", msg, "Save", "Don't Save", "Cancel",
                                       ACT_QUIT_SAVEPROMPT, i);
                    return;
                }
            }
            S.quit = 1;
            return;
        }
        default: return;
    }
}

static void dialogSubmitInput(void) {
    Dialog d = S.dlg;
    char text[512];
    strncpy(text, d.input, sizeof(text) - 1);
    text[sizeof(text)-1] = '\0';
    closeDialog();
    if (text[0] == '\0') { setStatus("Cancelled"); return; }

    switch (d.action) {
        case ACT_SAVE_AS_INPUT: {
            if (d.ctx_int >= 0 && d.ctx_int < S.ntabs) editorSaveAs(&S.tabs[d.ctx_int], text);
            break;
        }
        case ACT_RENAME_UNNAMED_ON_SAVE: {
            if (d.ctx_int >= 0 && d.ctx_int < S.ntabs) {
                editorSaveAs(&S.tabs[d.ctx_int], text);
                closeTabForceIndex(d.ctx_int);
            }
            break;
        }
        case ACT_NEW_FILE_INPUT: {
            char path[PATH_MAX];
            if (S.have_root && text[0] != '/') snprintf(path, sizeof(path), "%s/%s", S.root.path, text);
            else strncpy(path, text, sizeof(path) - 1);
            FILE *fp = fopen(path, "a");
            if (fp) fclose(fp);
            editorOpen(path, 1);
            if (S.have_root) explorerRebuildFlat();
            break;
        }
        default: break;
    }
}


/* =============================== command palette ============================ */
typedef struct { const char *name; const char *desc; } Command;
static Command COMMANDS[] = {
    { "Toggle Explorer",        "Ctrl+E" },
    { "Toggle Terminal",        "Ctrl+T" },
    { "Save",                   "Ctrl+S" },
    { "Save As",                "" },
    { "New File",                "Ctrl+N" },
    { "Close Tab",               "Ctrl+W" },
    { "Next Tab",                "Ctrl+Right" },
    { "Previous Tab",            "Ctrl+Left" },
    { "Toggle Theme (Dark/Light)","" },
    { "Cycle Focus",             "Ctrl+\\" },
    { "Quit",                    "Ctrl+Q" },
    { "Help",                    "" },
};
#define NCOMMANDS (int)(sizeof(COMMANDS)/sizeof(COMMANDS[0]))

static int paletteMatches[NCOMMANDS];
static int paletteNMatches;

static void paletteFilter(void) {
    paletteNMatches = 0;
    for (int i = 0; i < NCOMMANDS; i++) {
        if (S.palette_len == 0) { paletteMatches[paletteNMatches++] = i; continue; }
        /* simple case-insensitive substring match */
        char hay[128], needle[256];
        int hl = 0;
        for (const char *p = COMMANDS[i].name; *p && hl < 127; p++) hay[hl++] = (char)tolower((unsigned char)*p);
        hay[hl] = '\0';
        int nl = 0;
        for (int k = 0; k < S.palette_len && nl < 255; k++) needle[nl++] = (char)tolower((unsigned char)S.palette_query[k]);
        needle[nl] = '\0';
        if (strstr(hay, needle)) paletteMatches[paletteNMatches++] = i;
    }
    if (S.palette_sel >= paletteNMatches) S.palette_sel = paletteNMatches > 0 ? paletteNMatches - 1 : 0;
}

static void openPalette(void) {
    S.palette_active = 1;
    S.palette_len = 0;
    S.palette_query[0] = '\0';
    S.palette_sel = 0;
    S.focus_before_palette = S.focus;
    paletteFilter();
}

static void closePalette(void) {
    S.palette_active = 0;
    S.focus = S.focus_before_palette;
}

static void newUnnamedTab(void) {
    int idx = newTab(NULL);
    if (idx >= 0) S.focus = FOCUS_EDITOR;
}

static void executeCommand(const char *cmd) {
    if (!strcmp(cmd, "Toggle Explorer")) {
        S.show_explorer = !S.show_explorer;
    } else if (!strcmp(cmd, "Toggle Terminal")) {
        S.show_terminal = !S.show_terminal;
    } else if (!strcmp(cmd, "Save")) {
        editorSave(curBuf());
    } else if (!strcmp(cmd, "Save As")) {
        EBuffer *b = curBuf();
        if (b) openInputDialog("Save As", "Enter filename to save this file:", b->filename ? b->filename : "", ACT_SAVE_AS_INPUT, S.curtab);
    } else if (!strcmp(cmd, "New File")) {
        openInputDialog("New File", "Enter name for the new file:", "", ACT_NEW_FILE_INPUT, 0);
    } else if (!strcmp(cmd, "Close Tab")) {
        if (S.ntabs > 0) closeTab(S.curtab);
    } else if (!strcmp(cmd, "Next Tab")) {
        if (S.ntabs > 0) S.curtab = (S.curtab + 1) % S.ntabs;
    } else if (!strcmp(cmd, "Previous Tab")) {
        if (S.ntabs > 0) S.curtab = (S.curtab - 1 + S.ntabs) % S.ntabs;
    } else if (!strcmp(cmd, "Toggle Theme (Dark/Light)")) {
        S.theme = !S.theme;
    } else if (!strcmp(cmd, "Cycle Focus")) {
        do { S.focus = (S.focus + 1) % 3; }
        while ((S.focus == FOCUS_EXPLORER && !S.show_explorer) ||
               (S.focus == FOCUS_TERMINAL && !S.show_terminal));
    } else if (!strcmp(cmd, "Quit")) {
        requestQuit();
    } else if (!strcmp(cmd, "Help")) {
        openMessageDialog("vex Help",
            "Ctrl+E explorer  Ctrl+T terminal  Ctrl+P palette\n"
            "Ctrl+S save      Ctrl+N new file  Ctrl+W close tab\n"
            "Ctrl+\\ cycle focus   Ctrl+Q quit\n"
            "Click files/folders in the explorer to open/expand them.");
    }
}


/* ================================== layout =================================== */
static void computeLayout(void) {
    S.topbar_h = 1;
    int contentTop = S.topbar_h;
    int contentBot = S.screenrows; /* exclusive */

    S.term_h = 0;
    if (S.show_terminal) {
        S.term_h = S.terminal_height;
        if (S.term_h < TERM_MIN_H) S.term_h = TERM_MIN_H;
        if (S.term_h > S.screenrows - contentTop - 4) S.term_h = S.screenrows - contentTop - 4;
        if (S.term_h < 3) S.term_h = 3;
    }
    S.term_y = contentBot - S.term_h;
    S.term_x = 0;
    S.term_w = S.screencols;

    int mainBot = S.show_terminal ? S.term_y : contentBot;

    S.explorer_x = 0;
    S.explorer_y = contentTop;
    S.explorer_h = mainBot - contentTop;
    S.explorer_width = S.show_explorer ? (S.screencols / 5 < EXPLORER_MIN_W ? EXPLORER_MIN_W : S.screencols / 5) : 0;
    if (S.explorer_width > S.screencols - 20) S.explorer_width = S.screencols - 20;
    if (S.explorer_width < 0) S.explorer_width = 0;

    S.editor_x = S.show_explorer ? S.explorer_width + 1 : 0;
    S.editor_y = contentTop;
    S.editor_w = S.screencols - S.editor_x;
    S.editor_h = mainBot - contentTop;

    if (S.term_h > 0) termResize(&S.term, S.term_h - 1, S.term_w - 2);
}

/* row0..1 reserved inside editor for tab bar */
#define TABBAR_H 1

/* =============================== rendering helpers ============================ */
static void drawTopBar(abuf *ab) {
    RGB bg = THEME_TOPBAR_BG[S.theme];
    abGoto(ab, 0, 0);
    abBg(ab, bg);
    abFg(ab, THEME_EDITOR_FG[S.theme]);
    char buf[512];
    if (S.palette_active) {
        snprintf(buf, sizeof(buf), " \xEE\x82\xB0  %.*s%s", S.palette_len, S.palette_query, "");
    } else {
        snprintf(buf, sizeof(buf), " \xF0\x9F\x94\x8D  Ctrl+P: Command Palette   |   vex - Visual Editor eXperience   [%s]",
                 S.theme == THEME_DARK ? "Dark" : "Light");
    }
    int len = (int)strlen(buf);
    abAppendStr(ab, buf);
    for (int i = len; i < S.screencols; i++) abAppendStr(ab, " ");
    abReset(ab);
}

static const char *dirNodeIcon(DirNode *n) {
    if (n->is_dir) return n->expanded ? "\xF0\x9F\x93\x82" : "\xF0\x9F\x93\x81";
    return "\xF0\x9F\x93\x84";
}

static void drawExplorer(abuf *ab) {
    if (!S.show_explorer) return;
    RGB bg = THEME_EXPLORER_BG[S.theme];
    RGB fg = THEME_EDITOR_FG[S.theme];
    RGB selbg = THEME_EXPLORER_SEL_BG[S.theme];
    RGB border = THEME_BORDER[S.theme];

    for (int r = 0; r < S.explorer_h; r++) {
        abGoto(ab, S.explorer_y + r, 0);
        abBg(ab, bg); abFg(ab, fg);
        if (r == 0) {
            char hdr[64];
            snprintf(hdr, sizeof(hdr), " EXPLORER%s", S.focus == FOCUS_EXPLORER ? " *" : "");
            abAppendStr(ab, hdr);
            int hl = (int)strlen(hdr);
            for (int i = hl; i < S.explorer_width; i++) abAppendStr(ab, " ");
        } else {
            int flatIdx = r - 1 + S.explorer_scroll;
            if (S.have_root && flatIdx >= 0 && flatIdx < S.nflat) {
                DirNode *n = S.flat[flatIdx];
                int sel = (flatIdx == S.explorer_sel && S.focus == FOCUS_EXPLORER);
                if (sel) abBg(ab, selbg);
                char line[512];
                char indent[128]; int ind = 0;
                for (int d = 0; d < n->depth && ind < 120; d++) { indent[ind++]=' '; indent[ind++]=' '; }
                indent[ind] = '\0';
                snprintf(line, sizeof(line), " %s%s %s", indent, dirNodeIcon(n), n->name);
                abAppendStr(ab, line);
                int used = 1 + (int)strlen(line) - 1; /* approx display width (icons are multi-byte, close enough) */
                int visLen = 3 + ind + (int)strlen(n->name) + 2;
                (void)used;
                for (int i = visLen; i < S.explorer_width; i++) abAppendStr(ab, " ");
            } else {
                for (int i = 0; i < S.explorer_width; i++) abAppendStr(ab, " ");
            }
        }
        abReset(ab);
        abBg(ab, border); abFg(ab, border);
        abAppendStr(ab, "\xe2\x94\x82"); /* │ */
        abReset(ab);
    }
}

static void drawTabBar(abuf *ab) {
    abGoto(ab, S.editor_y, S.editor_x);
    int col = S.editor_x;
    for (int i = 0; i < S.ntabs && col < S.editor_x + S.editor_w; i++) {
        EBuffer *b = &S.tabs[i];
        const char *name = b->filename ? strrchr(b->filename, '/') ? strrchr(b->filename, '/') + 1 : b->filename : "Untitled";
        int active = (i == S.curtab);
        abBg(ab, active ? THEME_TAB_ACTIVE_BG[S.theme] : THEME_TAB_INACTIVE_BG[S.theme]);
        abFg(ab, active ? THEME_ACCENT[S.theme] : THEME_EDITOR_FG[S.theme]);
        char label[128];
        snprintf(label, sizeof(label), " %s%s ", name, b->dirty ? "*" : "");
        int llen = (int)strlen(label);
        if (col + llen > S.editor_x + S.editor_w) llen = S.editor_x + S.editor_w - col;
        if (llen > 0) abAppend(ab, label, llen);
        col += llen;
        abReset(ab);
    }
    abBg(ab, THEME_TAB_INACTIVE_BG[S.theme]);
    for (; col < S.editor_x + S.editor_w; col++) abAppendStr(ab, " ");
    abReset(ab);
}


/* =========================== editor content rendering =========================== */
static int rowCxToRx(erow *row, int cx) {
    int rx = 0;
    for (int j = 0; j < cx && j < row->size; j++) {
        if (row->chars[j] == '\t') rx += (TAB_STOP - 1) - (rx % TAB_STOP);
        rx++;
    }
    return rx;
}

static void editorScroll(EBuffer *b, int viewRows, int viewCols) {
    b->rx = 0;
    if (b->cy < b->numrows) b->rx = rowCxToRx(&b->row[b->cy], b->cx);
    if (b->cy < b->rowoff) b->rowoff = b->cy;
    if (b->cy >= b->rowoff + viewRows) b->rowoff = b->cy - viewRows + 1;
    if (b->rx < b->coloff) b->coloff = b->rx;
    if (b->rx >= b->coloff + viewCols) b->coloff = b->rx - viewCols + 1;
    if (b->rowoff < 0) b->rowoff = 0;
    if (b->coloff < 0) b->coloff = 0;
}

static RGB hlColor(unsigned char hl) {
    return THEME_FG[S.theme][hl];
}

static void drawEditorArea(abuf *ab) {
    int contentY = S.editor_y + TABBAR_H;
    int contentH = S.editor_h - TABBAR_H - 1; /* -1 for status line */
    if (contentH < 0) contentH = 0;
    RGB bg = THEME_BG[S.theme];

    if (S.ntabs == 0) {
        for (int r = 0; r < contentH; r++) {
            abGoto(ab, contentY + r, S.editor_x);
            abBg(ab, bg); abFg(ab, THEME_EDITOR_FG[S.theme]);
            if (r == contentH / 2) {
                const char *msg = "No file open  --  Ctrl+P for commands, Ctrl+N for a new file";
                int pad = (S.editor_w - (int)strlen(msg)) / 2;
                for (int i = 0; i < pad; i++) abAppendStr(ab, " ");
                abAppendStr(ab, msg);
                for (int i = pad + (int)strlen(msg); i < S.editor_w; i++) abAppendStr(ab, " ");
            } else {
                for (int i = 0; i < S.editor_w; i++) abAppendStr(ab, " ");
            }
            abReset(ab);
        }
        return;
    }

    EBuffer *b = curBuf();
    int gutterW = 5;
    int viewCols = S.editor_w - gutterW;
    if (viewCols < 1) viewCols = 1;
    editorScroll(b, contentH, viewCols);

    for (int r = 0; r < contentH; r++) {
        int filerow = r + b->rowoff;
        abGoto(ab, contentY + r, S.editor_x);
        abBg(ab, bg);
        if (filerow < b->numrows) {
            abFg(ab, filerow == b->cy ? THEME_LINE_NUM_ACTIVE[S.theme] : THEME_LINE_NUM[S.theme]);
            abAppendFmt(ab, "%4d ", filerow + 1);
        } else {
            abFg(ab, THEME_LINE_NUM[S.theme]);
            abAppendStr(ab, "     ");
        }
        if (filerow < b->numrows) {
            erow *row = &b->row[filerow];
            int len = row->rsize - b->coloff;
            if (len < 0) len = 0;
            if (len > viewCols) len = viewCols;
            unsigned char lastHl = 255;
            for (int j = 0; j < len; j++) {
                int idx = j + b->coloff;
                unsigned char hl = row->hl ? row->hl[idx] : HL_NORMAL;
                if (hl != lastHl) { abFg(ab, hlColor(hl)); lastHl = hl; }
                char ch = row->render[idx];
                if ((unsigned char)ch < 32) ch = '?';
                abAppend(ab, &ch, 1);
            }
            for (int j = len; j < viewCols; j++) abAppendStr(ab, " ");
        } else {
            abFg(ab, THEME_EDITOR_FG[S.theme]);
            for (int j = 0; j < viewCols; j++) abAppendStr(ab, " ");
        }
        abReset(ab);
    }

    /* status line */
    abGoto(ab, contentY + contentH, S.editor_x);
    abBg(ab, THEME_STATUSBAR_BG[S.theme]);
    abFg(ab, THEME_STATUSBAR_FG[S.theme]);
    char left[256], right[128];
    snprintf(left, sizeof(left), " %s%s  %s ",
             b->filename ? b->filename : "Untitled",
             b->dirty ? " *" : "",
             b->syntax ? b->syntax->name : "Plain Text");
    snprintf(right, sizeof(right), "Ln %d, Col %d ", b->cy + 1, b->cx + 1);
    int ll = (int)strlen(left), rl = (int)strlen(right);
    abAppendStr(ab, left);
    for (int i = ll; i < S.editor_w - rl; i++) abAppendStr(ab, " ");
    abAppendStr(ab, right);
    abReset(ab);
}

static void drawTerminalPane(abuf *ab) {
    if (!S.show_terminal || S.term_h <= 0) return;
    RGB border = THEME_BORDER[S.theme];
    RGB bg = THEME_BG[S.theme];

    abGoto(ab, S.term_y, 0);
    abBg(ab, border); abFg(ab, THEME_EDITOR_FG[S.theme]);
    char hdr[64];
    snprintf(hdr, sizeof(hdr), " TERMINAL%s ", S.focus == FOCUS_TERMINAL ? " *" : "");
    abAppendStr(ab, "\xe2\x94\x8c"); /* ┌ */
    abAppendStr(ab, hdr);
    int used = 1 + (int)strlen(hdr);
    for (int i = used; i < S.term_w - 1; i++) abAppendStr(ab, "\xe2\x94\x80"); /* ─ */
    abAppendStr(ab, "\xe2\x94\x90"); /* ┐ */
    abReset(ab);

    TermEmu *t = &S.term;
    for (int r = 0; r < t->rows; r++) {
        abGoto(ab, S.term_y + 1 + r, 0);
        abBg(ab, border); abFg(ab, border);
        abAppendStr(ab, "\xe2\x94\x82");
        abReset(ab);
        unsigned char lastFg[3] = {255,255,255}, lastBg[3] = {255,255,255};
        int first = 1;
        for (int c = 0; c < t->cols; c++) {
            TCell *cell = &t->grid[r * t->cols + c];
            RGB fg = cell->use_def_fg ? THEME_EDITOR_FG[S.theme] : (RGB){cell->fg_r,cell->fg_g,cell->fg_b};
            RGB cbg = cell->use_def_bg ? bg : (RGB){cell->bg_r,cell->bg_g,cell->bg_b};
            if (first || fg.r!=lastFg[0]||fg.g!=lastFg[1]||fg.b!=lastFg[2]) { abFg(ab, fg); lastFg[0]=fg.r;lastFg[1]=fg.g;lastFg[2]=fg.b; }
            if (first || cbg.r!=lastBg[0]||cbg.g!=lastBg[1]||cbg.b!=lastBg[2]) { abBg(ab, cbg); lastBg[0]=cbg.r;lastBg[1]=cbg.g;lastBg[2]=cbg.b; }
            first = 0;
            char ch = (char)cell->ch;
            if (ch < 32) ch = ' ';
            abAppend(ab, &ch, 1);
        }
        abReset(ab);
        abBg(ab, border); abFg(ab, border);
        abAppendStr(ab, "\xe2\x94\x82");
        abReset(ab);
    }

    abGoto(ab, S.term_y + t->rows + 1, 0);
    abBg(ab, border); abFg(ab, border);
    abAppendStr(ab, "\xe2\x94\x94");
    for (int i = 1; i < S.term_w - 1; i++) abAppendStr(ab, "\xe2\x94\x80");
    abAppendStr(ab, "\xe2\x94\x98");
    abReset(ab);
}


/* ============================ popups: dialogs & palette ========================== */
static void drawBoxAt(abuf *ab, int y, int x, int h, int w, RGB border, RGB bg, const char *title) {
    RGB fg = THEME_EDITOR_FG[S.theme];
    abGoto(ab, y, x);
    abBg(ab, border); abFg(ab, bg);
    abAppendStr(ab, "\xe2\x95\x94"); /* ╔ */
    char t[128];
    snprintf(t, sizeof(t), " %s ", title);
    int tlen = (int)strlen(t);
    int pad = w - 2 - tlen;
    int padL = pad / 2, padR = pad - padL;
    abFg(ab, border);
    for (int i = 0; i < padL; i++) abAppendStr(ab, "\xe2\x95\x90");
    abBg(ab, border); abFg(ab, (RGB){255,255,255});
    abAppendStr(ab, t);
    abFg(ab, border); abBg(ab, border);
    for (int i = 0; i < padR; i++) abAppendStr(ab, "\xe2\x95\x90");
    abAppendStr(ab, "\xe2\x95\x97"); /* ╗ */
    abReset(ab);

    for (int r = 1; r < h - 1; r++) {
        abGoto(ab, y + r, x);
        abBg(ab, border); abFg(ab, border);
        abAppendStr(ab, "\xe2\x95\x91"); /* ║ */
        abBg(ab, bg); abFg(ab, fg);
        for (int i = 0; i < w - 2; i++) abAppendStr(ab, " ");
        abBg(ab, border); abFg(ab, border);
        abAppendStr(ab, "\xe2\x95\x91");
        abReset(ab);
    }

    abGoto(ab, y + h - 1, x);
    abBg(ab, border); abFg(ab, border);
    abAppendStr(ab, "\xe2\x95\x9a"); /* ╚ */
    for (int i = 0; i < w - 2; i++) abAppendStr(ab, "\xe2\x95\x90");
    abAppendStr(ab, "\xe2\x95\x9d"); /* ╝ */
    abReset(ab);
}

static void wrapText(const char *msg, int width, char lines[][256], int *nlines, int maxlines) {
    *nlines = 0;
    const char *p = msg;
    while (*p && *nlines < maxlines) {
        const char *nl = strchr(p, '\n');
        int seglen = nl ? (int)(nl - p) : (int)strlen(p);
        while (seglen > 0 && *nlines < maxlines) {
            int take = seglen > width ? width : seglen;
            if (seglen > width) {
                int cut = take;
                while (cut > 0 && p[cut] != ' ') cut--;
                if (cut > 0) take = cut;
            }
            snprintf(lines[*nlines], 256, "%.*s", take, p);
            (*nlines)++;
            p += take;
            seglen -= take;
            while (*p == ' ') { p++; seglen--; }
        }
        if (nl) p = nl + 1; else break;
    }
}

static void drawDialog(abuf *ab) {
    if (S.dlg.type == DLG_NONE) return;
    int w = 56;
    if (w > S.screencols - 4) w = S.screencols - 4;
    char lines[16][256]; int nlines;
    wrapText(S.dlg.message, w - 4, lines, &nlines, 8);
    int h = 4 + nlines + (S.dlg.type == DLG_INPUT ? 2 : 0);
    int y = (S.screenrows - h) / 2;
    int x = (S.screencols - w) / 2;
    if (y < 0) y = 0;
    if (x < 0) x = 0;

    RGB border = THEME_DIALOG_BORDER[S.theme];
    RGB bg = THEME_DIALOG_BG[S.theme];
    drawBoxAt(ab, y, x, h, w, border, bg, S.dlg.title);

    RGB fg = THEME_EDITOR_FG[S.theme];
    for (int i = 0; i < nlines; i++) {
        abGoto(ab, y + 1 + i, x + 2);
        abBg(ab, bg); abFg(ab, fg);
        abAppendStr(ab, lines[i]);
        abReset(ab);
    }

    if (S.dlg.type == DLG_INPUT) {
        int iy = y + 1 + nlines + 1;
        abGoto(ab, iy, x + 2);
        abBg(ab, THEME_BG[S.theme]); abFg(ab, fg);
        abAppendStr(ab, "> ");
        abAppendStr(ab, S.dlg.input);
        abAppendStr(ab, "\xe2\x96\x88"); /* cursor block */
        int used = 2 + S.dlg.input_len + 1;
        for (int i = used; i < w - 4; i++) abAppendStr(ab, " ");
        abReset(ab);
        abGoto(ab, y + h - 2, x + 2);
        abBg(ab, bg); abFg(ab, THEME_LINE_NUM[S.theme]);
        abAppendStr(ab, "[Enter] Confirm   [Esc] Cancel");
        abReset(ab);
    } else {
        int by = y + h - 2;
        int totalw = 0;
        for (int i = 0; i < S.dlg.nbuttons; i++) totalw += (int)strlen(S.dlg.buttons[i]) + 4;
        int bx = x + (w - totalw) / 2;
        abGoto(ab, by, bx);
        for (int i = 0; i < S.dlg.nbuttons; i++) {
            int sel = (i == S.dlg.selected);
            abBg(ab, sel ? THEME_ACCENT[S.theme] : bg);
            abFg(ab, sel ? (RGB){255,255,255} : fg);
            char lbl[32];
            snprintf(lbl, sizeof(lbl), " %s ", S.dlg.buttons[i]);
            abAppendStr(ab, lbl);
            abReset(ab);
            abAppendStr(ab, "  ");
        }
    }
}

static void drawPaletteDropdown(abuf *ab) {
    if (!S.palette_active) return;
    int w = S.screencols > 70 ? 60 : S.screencols - 4;
    int x = (S.screencols - w) / 2;
    int maxItems = paletteNMatches < 8 ? paletteNMatches : 8;
    if (maxItems < 1) maxItems = 1;
    int h = maxItems + 2;
    int y = 1;

    RGB border = THEME_DIALOG_BORDER[S.theme];
    RGB bg = THEME_DIALOG_BG[S.theme];
    drawBoxAt(ab, y, x, h, w, border, bg, "Command Palette");

    for (int i = 0; i < maxItems; i++) {
        abGoto(ab, y + 1 + i, x + 2);
        int cmdIdx = (i < paletteNMatches) ? paletteMatches[i] : -1;
        int sel = (i == S.palette_sel);
        abBg(ab, sel ? THEME_ACCENT[S.theme] : bg);
        abFg(ab, sel ? (RGB){255,255,255} : THEME_EDITOR_FG[S.theme]);
        if (cmdIdx >= 0) {
            char line[256];
            snprintf(line, sizeof(line), "%-38s %10s", COMMANDS[cmdIdx].name, COMMANDS[cmdIdx].desc);
            int ll = (int)strlen(line);
            if (ll > w - 4) ll = w - 4;
            abAppend(ab, line, ll);
            for (int k = ll; k < w - 4; k++) abAppendStr(ab, " ");
        } else {
            for (int k = 0; k < w - 4; k++) abAppendStr(ab, " ");
        }
        abReset(ab);
    }
}


/* ================================= refresh ==================================== */
static void refreshScreen(void) {
    abuf ab; abInit(&ab);
    abAppendStr(&ab, "\x1b[?25l");
    abBg(&ab, THEME_BG[S.theme]);
    abAppendStr(&ab, "\x1b[2J");

    drawTopBar(&ab);
    drawExplorer(&ab);
    drawTabBar(&ab);
    drawEditorArea(&ab);
    drawTerminalPane(&ab);
    drawDialog(&ab);
    drawPaletteDropdown(&ab);

    /* place terminal cursor */
    int cy = 0, cx = 0;
    if (S.dlg.type == DLG_INPUT) {
        int w = 56; if (w > S.screencols - 4) w = S.screencols - 4;
        char lines[16][256]; int nlines;
        wrapText(S.dlg.message, w - 4, lines, &nlines, 8);
        int h = 4 + nlines + 2;
        int y = (S.screenrows - h) / 2, x = (S.screencols - w) / 2;
        if (y < 0) y = 0; if (x < 0) x = 0;
        cy = y + 1 + nlines + 1;
        cx = x + 2 + 2 + S.dlg.input_len;
    } else if (S.palette_active) {
        cy = 0; cx = 6 + S.palette_len;
    } else if (S.focus == FOCUS_EDITOR && S.ntabs > 0) {
        EBuffer *b = curBuf();
        int contentY = S.editor_y + TABBAR_H;
        cy = contentY + (b->cy - b->rowoff);
        cx = S.editor_x + 5 + (b->rx - b->coloff);
    } else if (S.focus == FOCUS_TERMINAL && S.show_terminal) {
        cy = S.term_y + 1 + S.term.cy;
        cx = 1 + S.term.cx;
    } else {
        cy = 0; cx = 0;
    }
    if (cy < 0) cy = 0; if (cy >= S.screenrows) cy = S.screenrows - 1;
    if (cx < 0) cx = 0; if (cx >= S.screencols) cx = S.screencols - 1;
    abAppendFmt(&ab, "\x1b[%d;%dH", cy + 1, cx + 1);
    abAppendStr(&ab, "\x1b[?25h");

    write(STDOUT_FILENO, ab.b, (size_t)ab.len);
    abFree(&ab);
}


/* ================================= input reading ================================ */
typedef struct { int key; int mx, my; int mbutton; int mrelease; } InputEvent;

static int readRawByte(unsigned char *c) {
    ssize_t n = read(STDIN_FILENO, c, 1);
    if (n == 1) return 1;
    return 0;
}

/* Parses one input event from stdin (non-blocking; caller ensures data is ready
 * via select()). Handles escape sequences for arrows/nav keys and SGR mouse. */
static int readInputEvent(InputEvent *ev) {
    unsigned char c;
    if (!readRawByte(&c)) return 0;
    ev->key = 0; ev->mx = ev->my = ev->mbutton = ev->mrelease = -1;

    if (c != 0x1b) { ev->key = c; return 1; }

    unsigned char seq[32]; int slen = 0;
    /* try to read the rest of the escape sequence; small timeout loop */
    for (int i = 0; i < 20; i++) {
        unsigned char nc;
        if (readRawByte(&nc)) { seq[slen++] = nc; if (slen >= (int)sizeof(seq) - 1) break; }
        else {
            struct timespec ts = {0, 1000000}; /* 1ms */
            nanosleep(&ts, NULL);
            if (slen > 0 && !isdigit(seq[slen-1]) && seq[slen-1] != ';' && seq[slen-1] != '[' && seq[slen-1] != 'O') break;
            if (slen == 0) continue;
        }
        if (slen > 0) {
            unsigned char last = seq[slen - 1];
            if ((last >= 'A' && last <= 'Z') || (last >= 'a' && last <= 'z') || last == '~') break;
        }
    }
    if (slen == 0) { ev->key = KEY_ESCAPE_ALONE; return 1; }

    if (seq[0] == '[') {
        if (slen >= 2 && seq[1] == '<') {
            /* SGR mouse: ESC [ < b ; x ; y (M|m) */
            int b=0,x=0,y=0; int i=2; int *cur=&b; int neg=0; (void)neg;
            for (; i < slen; i++) {
                if (seq[i] == ';') { cur = (cur == &b) ? &x : &y; }
                else if (seq[i] >= '0' && seq[i] <= '9') { *cur = *cur * 10 + (seq[i]-'0'); }
                else if (seq[i] == 'M' || seq[i] == 'm') {
                    ev->key = KEY_MOUSE;
                    ev->mbutton = b;
                    ev->mx = x - 1; ev->my = y - 1;
                    ev->mrelease = (seq[i] == 'm');
                    return 1;
                }
            }
            ev->key = 0; return 1;
        }
        if (slen == 1) {
            switch (seq[0]) {
                case 'A': ev->key = KEY_ARROW_UP; return 1;
                case 'B': ev->key = KEY_ARROW_DOWN; return 1;
                case 'C': ev->key = KEY_ARROW_RIGHT; return 1;
                case 'D': ev->key = KEY_ARROW_LEFT; return 1;
                case 'H': ev->key = KEY_HOME; return 1;
                case 'F': ev->key = KEY_END; return 1;
            }
        }
        /* Ctrl+Arrow: ESC [ 1 ; 5 (A-D)   Also plain PgUp/PgDn/Del/Home/End with ~ */
        if (slen >= 4 && seq[0]=='1' && seq[1]==';' && seq[2]=='5') {
            char f = seq[3];
            if (f=='C') { ev->key = KEY_CTRL_RIGHT; return 1; }
            if (f=='D') { ev->key = KEY_CTRL_LEFT; return 1; }
            if (f=='A') { ev->key = KEY_ARROW_UP; return 1; }
            if (f=='B') { ev->key = KEY_ARROW_DOWN; return 1; }
        }
        if (seq[slen-1] == '~') {
            char numbuf[8]; int ni=0;
            for (int i=0;i<slen-1 && ni<7;i++) if (isdigit(seq[i])) numbuf[ni++]=(char)seq[i];
            numbuf[ni]='\0';
            int code = ni>0 ? atoi(numbuf) : 0;
            switch (code) {
                case 1: ev->key = KEY_HOME; return 1;
                case 3: ev->key = KEY_DEL; return 1;
                case 4: ev->key = KEY_END; return 1;
                case 5: ev->key = KEY_PAGE_UP; return 1;
                case 6: ev->key = KEY_PAGE_DOWN; return 1;
                case 7: ev->key = KEY_HOME; return 1;
                case 8: ev->key = KEY_END; return 1;
            }
        }
    } else if (seq[0] == 'O') {
        if (slen == 2) {
            switch (seq[1]) {
                case 'H': ev->key = KEY_HOME; return 1;
                case 'F': ev->key = KEY_END; return 1;
            }
        }
    }
    ev->key = KEY_ESCAPE_ALONE;
    return 1;
}


/* ================================ input dispatch ================================= */
#define CTRL_KEY(k) ((k) & 0x1f)

static void moveCursor(EBuffer *b, int key) {
    erow *row = (b->cy < b->numrows) ? &b->row[b->cy] : NULL;
    switch (key) {
        case KEY_ARROW_LEFT:
            if (b->cx > 0) b->cx--;
            else if (b->cy > 0) { b->cy--; b->cx = b->row[b->cy].size; }
            break;
        case KEY_ARROW_RIGHT:
            if (row && b->cx < row->size) b->cx++;
            else if (row && b->cy < b->numrows - 1) { b->cy++; b->cx = 0; }
            break;
        case KEY_ARROW_UP: if (b->cy > 0) b->cy--; break;
        case KEY_ARROW_DOWN: if (b->cy < b->numrows - 1) b->cy++; break;
        case KEY_HOME: b->cx = 0; break;
        case KEY_END: if (row) b->cx = row->size; break;
    }
    row = (b->cy < b->numrows) ? &b->row[b->cy] : NULL;
    int rowlen = row ? row->size : 0;
    if (b->cx > rowlen) b->cx = rowlen;
}

static void handleEditorKey(EBuffer *b, InputEvent *ev) {
    int key = ev->key;
    if (key == '\r' || key == '\n') { editorInsertNewline(b); return; }
    if (key == KEY_BACKSPACE || key == CTRL_KEY('h')) { editorDelChar(b); return; }
    if (key == KEY_DEL) { editorDelForward(b); return; }
    if (key == KEY_ARROW_LEFT || key == KEY_ARROW_RIGHT || key == KEY_ARROW_UP ||
        key == KEY_ARROW_DOWN || key == KEY_HOME || key == KEY_END) { moveCursor(b, key); return; }
    if (key == KEY_PAGE_UP) { b->cy -= S.editor_h; if (b->cy<0) b->cy=0; return; }
    if (key == KEY_PAGE_DOWN) { b->cy += S.editor_h; if (b->cy>=b->numrows) b->cy=b->numrows-1; if(b->cy<0)b->cy=0; return; }
    if (key == '\t') { editorInsertChar(b, ' '); editorInsertChar(b, ' '); return; }
    if (key >= 32 && key < 127) { editorInsertChar(b, key); return; }
}

static void handleMouseClick(InputEvent *ev) {
    if (ev->mrelease) return; /* only act on press */
    int button = ev->mbutton & 3;
    if (button == 3) return; /* release-as-3 in some terminals; ignore */
    int mx = ev->mx, my = ev->my;

    if (my == 0) { S.focus = FOCUS_EDITOR; if (!S.palette_active) openPalette(); return; }

    if (S.show_explorer && mx < S.explorer_width && my >= S.explorer_y && my < S.explorer_y + S.explorer_h) {
        S.focus = FOCUS_EXPLORER;
        if (my == S.explorer_y) return;
        int flatIdx = my - S.explorer_y - 1 + S.explorer_scroll;
        explorerToggleOrOpen(flatIdx);
        return;
    }

    if (my == S.editor_y && mx >= S.editor_x) {
        int col = S.editor_x;
        for (int i = 0; i < S.ntabs; i++) {
            EBuffer *b = &S.tabs[i];
            const char *name = b->filename ? (strrchr(b->filename, '/') ? strrchr(b->filename, '/') + 1 : b->filename) : "Untitled";
            int llen = (int)strlen(name) + (b->dirty?1:0) + 2;
            if (mx >= col && mx < col + llen) { S.curtab = i; S.focus = FOCUS_EDITOR; return; }
            col += llen;
        }
        return;
    }

    if (mx >= S.editor_x && my >= S.editor_y + TABBAR_H && my < S.editor_y + S.editor_h - 1) {
        S.focus = FOCUS_EDITOR;
        if (S.ntabs == 0) return;
        EBuffer *b = curBuf();
        int contentY = S.editor_y + TABBAR_H;
        int filerow = (my - contentY) + b->rowoff;
        if (filerow >= b->numrows) filerow = b->numrows - 1;
        if (filerow < 0) filerow = 0;
        b->cy = filerow;
        int gutterW = 5;
        int col = (mx - S.editor_x - gutterW) + b->coloff;
        if (col < 0) col = 0;
        erow *row = &b->row[b->cy];
        if (col > row->size) col = row->size;
        b->cx = col;
        return;
    }

    if (S.show_terminal && my >= S.term_y && my < S.term_y + S.term_h) {
        S.focus = FOCUS_TERMINAL;
        return;
    }
}

static int globalKeyHandled(int key) {
    switch (key) {
        case CTRL_KEY('e'): S.show_explorer = !S.show_explorer; return 1;
        case CTRL_KEY('t'): S.show_terminal = !S.show_terminal; return 1;
        case CTRL_KEY('p'): if (!S.palette_active) openPalette(); else closePalette(); return 1;
        case CTRL_KEY('s'): editorSave(curBuf()); return 1;
        case CTRL_KEY('n'): openInputDialog("New File", "Enter name for the new file:", "", ACT_NEW_FILE_INPUT, 0); return 1;
        case CTRL_KEY('w'): if (S.ntabs > 0) closeTab(S.curtab); return 1;
        case CTRL_KEY('q'): requestQuit(); return 1;
        case CTRL_KEY('\\'): executeCommand("Cycle Focus"); return 1;
        case KEY_CTRL_RIGHT: if (S.ntabs > 0) S.curtab = (S.curtab + 1) % S.ntabs; return 1;
        case KEY_CTRL_LEFT: if (S.ntabs > 0) S.curtab = (S.curtab - 1 + S.ntabs) % S.ntabs; return 1;
    }
    return 0;
}

static void processEvent(InputEvent *ev) {
    if (ev->key == KEY_MOUSE) { handleMouseClick(ev); return; }

    /* Dialog has highest priority */
    if (S.dlg.type != DLG_NONE) {
        int key = ev->key;
        if (S.dlg.type == DLG_INPUT) {
            if (key == '\r' || key == '\n') { dialogSubmitInput(); return; }
            if (key == KEY_ESCAPE_ALONE || key == 27) { closeDialog(); return; }
            if (key == KEY_BACKSPACE) { if (S.dlg.input_len > 0) S.dlg.input[--S.dlg.input_len] = '\0'; return; }
            if (key >= 32 && key < 127 && S.dlg.input_len < (int)sizeof(S.dlg.input) - 1) {
                S.dlg.input[S.dlg.input_len++] = (char)key;
                S.dlg.input[S.dlg.input_len] = '\0';
            }
            return;
        } else { /* DLG_CONFIRM or DLG_MESSAGE */
            if (key == KEY_ARROW_LEFT) { if (S.dlg.selected > 0) S.dlg.selected--; return; }
            if (key == KEY_ARROW_RIGHT || key == '\t') { if (S.dlg.selected < S.dlg.nbuttons - 1) S.dlg.selected++; return; }
            if (key == '\r' || key == '\n') { dialogConfirm(S.dlg.selected); return; }
            if (key == KEY_ESCAPE_ALONE || key == 27) { dialogConfirm(S.dlg.nbuttons - 1); return; }
            if (key == 'y' || key == 'Y') { dialogConfirm(0); return; }
            if (key == 'n' || key == 'N') { dialogConfirm(S.dlg.nbuttons >= 2 ? 1 : 0); return; }
            return;
        }
    }

    if (S.palette_active) {
        int key = ev->key;
        if (key == KEY_ESCAPE_ALONE || key == 27) { closePalette(); return; }
        if (key == '\r' || key == '\n') {
            if (paletteNMatches > 0) {
                const char *cmd = COMMANDS[paletteMatches[S.palette_sel]].name;
                closePalette();
                executeCommand(cmd);
            } else closePalette();
            return;
        }
        if (key == KEY_ARROW_DOWN) { if (S.palette_sel < paletteNMatches - 1) S.palette_sel++; return; }
        if (key == KEY_ARROW_UP) { if (S.palette_sel > 0) S.palette_sel--; return; }
        if (key == KEY_BACKSPACE) { if (S.palette_len > 0) { S.palette_query[--S.palette_len] = '\0'; paletteFilter(); } return; }
        if (key >= 32 && key < 127 && S.palette_len < (int)sizeof(S.palette_query) - 1) {
            S.palette_query[S.palette_len++] = (char)ev->key;
            S.palette_query[S.palette_len] = '\0';
            paletteFilter();
            return;
        }
        return;
    }

    if (globalKeyHandled(ev->key)) return;

    if (S.focus == FOCUS_TERMINAL && S.show_terminal) {
        int key = ev->key;
        if (key == KEY_ARROW_UP) { termWrite("\x1b[A", 3); return; }
        if (key == KEY_ARROW_DOWN) { termWrite("\x1b[B", 3); return; }
        if (key == KEY_ARROW_RIGHT) { termWrite("\x1b[C", 3); return; }
        if (key == KEY_ARROW_LEFT) { termWrite("\x1b[D", 3); return; }
        if (key == KEY_BACKSPACE) { termWrite("\x7f", 1); return; }
        if (key == '\r' || key == '\n') { termWrite("\r", 1); return; }
        if (key == KEY_ESCAPE_ALONE) { termWrite("\x1b", 1); return; }
        if (key >= 0 && key < 256) { char c = (char)key; termWrite(&c, 1); return; }
        return;
    }

    if (S.focus == FOCUS_EXPLORER && S.show_explorer) {
        int key = ev->key;
        if (key == KEY_ARROW_DOWN) { if (S.explorer_sel < S.nflat - 1) S.explorer_sel++; return; }
        if (key == KEY_ARROW_UP) { if (S.explorer_sel > 0) S.explorer_sel--; return; }
        if (key == '\r' || key == '\n') { explorerToggleOrOpen(S.explorer_sel); return; }
        return;
    }

    if (S.focus == FOCUS_EDITOR) {
        if (S.ntabs == 0) {
            if (ev->key == CTRL_KEY('n')) newUnnamedTab();
            return;
        }
        handleEditorKey(curBuf(), ev);
        return;
    }
}


/* ==================================== main ==================================== */
static void applyResize(void) {
    int rows, cols;
    if (getWindowSize(&rows, &cols) == -1) return;
    S.screenrows = rows;
    S.screencols = cols;
    computeLayout();
}

int main(int argc, char **argv) {
    memset(&S, 0, sizeof(S));
    initThemes();
    S.theme = THEME_DARK;
    S.show_explorer = 1;
    S.show_terminal = 1;
    S.terminal_height = 12;
    S.focus = FOCUS_EDITOR;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handleSigwinch;
    sigaction(SIGWINCH, &sa, NULL);
    struct sigaction sc;
    memset(&sc, 0, sizeof(sc));
    sc.sa_handler = handleSigchld;
    sc.sa_flags = SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sc, NULL);
    signal(SIGPIPE, SIG_IGN);

    enableRawMode();
    atexit(disableRawMode);

    if (getWindowSize(&S.screenrows, &S.screencols) == -1) {
        S.screenrows = 24; S.screencols = 80;
    }
    computeLayout();

    /* Determine root directory / initial file from argv */
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) strcpy(cwd, ".");

    if (argc > 1) {
        struct stat st;
        if (stat(argv[1], &st) == 0 && S_ISDIR(st.st_mode)) {
            explorerSetRoot(argv[1]);
        } else {
            char dirpart[PATH_MAX];
            strncpy(dirpart, argv[1], sizeof(dirpart) - 1);
            char *slash = strrchr(dirpart, '/');
            if (slash) { *slash = '\0'; explorerSetRoot(dirpart[0] ? dirpart : "."); }
            else explorerSetRoot(cwd);
            editorOpen(argv[1], 1);
        }
    } else {
        explorerSetRoot(cwd);
    }

    if (S.ntabs == 0) newUnnamedTab();

    termStartShell(&S.term, S.term_h > 1 ? S.term_h - 1 : 10, S.screencols - 2);
    computeLayout();

    setStatus("vex %s  --  Ctrl+P for commands, Ctrl+E explorer, Ctrl+T terminal", VEX_VERSION);

    unsigned char termbuf[8192];

    while (!S.quit) {
        if (S.resized) { S.resized = 0; applyResize(); }
        if (S.sigchld_flag) {
            S.sigchld_flag = 0;
            int status;
            pid_t r = waitpid(S.term.child_pid, &status, WNOHANG);
            if (r == S.term.child_pid) S.term.alive = 0;
        }

        refreshScreen();

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds);
        int maxfd = STDIN_FILENO;
        if (S.term.alive) {
            FD_SET(S.term.master_fd, &rfds);
            if (S.term.master_fd > maxfd) maxfd = S.term.master_fd;
        }
        struct timeval tv; tv.tv_sec = 0; tv.tv_usec = 150000;
        int rv = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (rv < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (rv == 0) continue;

        if (S.term.alive && FD_ISSET(S.term.master_fd, &rfds)) {
            ssize_t n = read(S.term.master_fd, termbuf, sizeof(termbuf));
            if (n > 0) termFeed(&S.term, termbuf, (size_t)n);
            else if (n == 0 || (n < 0 && errno != EAGAIN)) S.term.alive = 0;
        }

        if (FD_ISSET(STDIN_FILENO, &rfds)) {
            InputEvent ev;
            while (readInputEvent(&ev)) {
                processEvent(&ev);
                /* drain any further immediately-available bytes without blocking */
                fd_set r2; FD_ZERO(&r2); FD_SET(STDIN_FILENO, &r2);
                struct timeval tv0 = {0,0};
                if (select(STDIN_FILENO + 1, &r2, NULL, NULL, &tv0) <= 0) break;
            }
        }
    }

    if (S.term.alive && S.term.child_pid > 0) kill(S.term.child_pid, SIGHUP);
    disableRawMode();
    return 0;
}
