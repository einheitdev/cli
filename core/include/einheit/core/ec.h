/// @file ec.h
/// @brief einheit_core — single-header portable CLI framework.
///
/// Minimal CLI framework for embedded products. Provides the
/// einheit CLI "feel" (prefix matching, tab completion, semantic
/// tables, role gating) in pure C11 with no heap allocation.
///
/// Tuned for RP2040 (264 KB SRAM, 2 MB flash) but works on any
/// target with a C11 compiler and ~8 KB of stack.
// Copyright (c) 2026 Einheit Networks

#ifndef EINHEIT_CORE_EC_H_
#define EINHEIT_CORE_EC_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

// ── Tunable limits ──────────────────────────────────────────
// Override before including this header for tighter builds.

#ifndef EC_MAX_TOKEN
#define EC_MAX_TOKEN 48
#endif

#ifndef EC_MAX_PATH_TOKENS
#define EC_MAX_PATH_TOKENS 4
#endif

#ifndef EC_MAX_ARGS
#define EC_MAX_ARGS 4
#endif

#ifndef EC_MAX_FLAGS
#define EC_MAX_FLAGS 4
#endif

#ifndef EC_MAX_COMMANDS
#define EC_MAX_COMMANDS 32
#endif

#ifndef EC_MAX_COMPLETIONS
#define EC_MAX_COMPLETIONS 16
#endif

#ifndef EC_TABLE_MAX_COLUMNS
#define EC_TABLE_MAX_COLUMNS 8
#endif

#ifndef EC_TABLE_MAX_ROWS
#define EC_TABLE_MAX_ROWS 32
#endif

#ifndef EC_TABLE_MAX_TEXT
#define EC_TABLE_MAX_TEXT 48
#endif

#ifndef EC_LINE_MAX
#define EC_LINE_MAX 128
#endif

#ifndef EC_HISTORY_SIZE
#define EC_HISTORY_SIZE 16
#endif

// ── Output buffer ───────────────────────────────────────────

/// Caller-owned output buffer. Functions write into this and
/// advance `used`. Always NUL-terminates.
typedef struct {
  char *data;
  size_t cap;
  size_t used;
} ec_buf;

size_t ec_buf_append(ec_buf *buf, const char *src, size_t n);
size_t ec_buf_puts(ec_buf *buf, const char *s);
size_t ec_buf_printf(ec_buf *buf, const char *fmt, ...);
void ec_buf_clear(ec_buf *buf);

// ── Errors ──────────────────────────────────────────────────

typedef enum {
  EC_OK = 0,
  EC_ERR_DUPLICATE_REGISTRATION,
  EC_ERR_UNKNOWN_COMMAND,
  EC_ERR_MISSING_ARGUMENT,
  EC_ERR_INVALID_ARGUMENT,
  EC_ERR_NOT_AUTHORISED,
  EC_MODE_CHANGED,
} ec_error_code;

typedef struct {
  ec_error_code code;
  char message[80];
} ec_error;

void ec_error_set(ec_error *err, ec_error_code code,
                  const char *fmt, ...);

// ── Roles ───────────────────────────────────────────────────

typedef enum {
  EC_ROLE_ANY = 0,
  EC_ROLE_OPERATOR = 1,
  EC_ROLE_ADMIN = 2,
} ec_role_gate;

const char *ec_role_label(ec_role_gate role);
bool ec_role_allows(ec_role_gate caller, ec_role_gate required);

// ── Fuzzy matching ──────────────────────────────────────────

size_t ec_fuzzy_distance(const char *a, size_t len_a,
                         const char *b, size_t len_b);

size_t ec_fuzzy_suggest(const char *query,
                        const char *const *vocab,
                        size_t vocab_count,
                        char out[][EC_MAX_TOKEN],
                        size_t out_cap);

// ── Command tree ────────────────────────────────────────────

typedef struct {
  char name[EC_MAX_TOKEN];
  char help[64];
  bool required;
} ec_arg_spec;

typedef struct {
  char path[80];
  ec_arg_spec args[EC_MAX_ARGS];
  size_t arg_count;
  ec_role_gate role;
  char wire_command[EC_MAX_TOKEN];
  char help[80];

  // Internal: populated by ec_ct_register.
  char path_tokens[EC_MAX_PATH_TOKENS][EC_MAX_TOKEN];
  size_t path_token_count;
} ec_command_spec;

typedef struct {
  ec_command_spec specs[EC_MAX_COMMANDS];
  size_t count;
} ec_command_tree;

typedef struct {
  const ec_command_spec *spec;
  char args[EC_MAX_ARGS][EC_MAX_TOKEN];
  size_t arg_count;
} ec_parsed_command;

void ec_ct_init(ec_command_tree *tree);

ec_error_code ec_ct_register(ec_command_tree *tree,
                             const ec_command_spec *spec);

ec_error_code ec_ct_parse(const ec_command_tree *tree,
                          const char *const *tokens,
                          size_t token_count,
                          ec_role_gate caller_role,
                          ec_parsed_command *out,
                          ec_error *err);

size_t ec_ct_suggest(const ec_command_tree *tree,
                     const char *const *preceding,
                     size_t prec_count, const char *partial,
                     char out[][EC_MAX_TOKEN],
                     size_t out_cap);

size_t ec_ct_format_help(const ec_command_tree *tree,
                         ec_buf *buf);

size_t ec_ct_format_command(const ec_command_spec *spec,
                            ec_buf *buf);

// ── Semantic table ──────────────────────────────────────────

typedef enum {
  EC_SEM_DEFAULT = 0,
  EC_SEM_GOOD,
  EC_SEM_WARN,
  EC_SEM_BAD,
  EC_SEM_DIM,
  EC_SEM_EMPHASIS,
} ec_semantic;

typedef enum {
  EC_ALIGN_LEFT = 0,
  EC_ALIGN_RIGHT,
} ec_align;

typedef struct {
  char text[EC_TABLE_MAX_TEXT];
  ec_semantic semantic;
} ec_cell;

typedef struct {
  char header[EC_TABLE_MAX_TEXT];
  ec_align align;
} ec_column;

typedef struct {
  ec_column columns[EC_TABLE_MAX_COLUMNS];
  size_t column_count;
  ec_cell rows[EC_TABLE_MAX_ROWS][EC_TABLE_MAX_COLUMNS];
  size_t row_count;
} ec_table;

void ec_table_init(ec_table *t);
void ec_table_add_column(ec_table *t, const char *header,
                         ec_align align);
void ec_table_add_row(ec_table *t, const ec_cell *cells,
                      size_t count);
ec_cell ec_cell_make(const char *text, ec_semantic sem);
const char *ec_plain_marker(ec_semantic s);

/// Render table as plain text into buf. Fixed 80-col layout.
size_t ec_table_render(const ec_table *t, ec_buf *buf);

/// Render table as JSON array of objects.
size_t ec_table_render_json(const ec_table *t, ec_buf *buf);

// ── I/O callbacks ───────────────────────────────────────────
// The framework never calls printf or getchar directly. The
// consumer provides these two functions at startup.

typedef struct {
  /// Write a NUL-terminated string to the console.
  void (*puts)(const char *s);
  /// Read one character. Blocks until available. Returns the
  /// character, or -1 on EOF/error.
  int (*getc)(void);
} ec_io;

// ── Line reader ─────────────────────────────────────────────
// Minimal line editor with history and tab completion.

typedef struct {
  char lines[EC_HISTORY_SIZE][EC_LINE_MAX];
  size_t count;
  size_t oldest;
} ec_history;

void ec_history_init(ec_history *h);
void ec_history_add(ec_history *h, const char *line);

/// Read a line with editing, history, and tab completion.
/// @param io      I/O callbacks.
/// @param prompt  Prompt string to display.
/// @param tree    Command tree for tab completion (or NULL).
/// @param hist    History for up/down (or NULL).
/// @param buf     Output buffer for the line.
/// @param buf_size Size of buf.
/// @returns Length of line, or -1 on EOF.
int ec_readline(const ec_io *io, const char *prompt,
                const ec_command_tree *tree,
                ec_history *hist,
                char *buf, size_t buf_size);

// ── Shell ───────────────────────────────────────────────────
// Ties everything together into a REPL.

/// Callback invoked for each parsed command. The adapter
/// implements this to dispatch to hardware.
/// Return EC_OK, or an error code. Write output via io->puts.
typedef ec_error_code (*ec_dispatch_fn)(
    const ec_parsed_command *cmd,
    const ec_io *io,
    void *user_data);

/// Optional callback invoked during idle time (between
/// keystrokes). Use for periodic tasks like loop detection.
/// Called roughly every 100ms when getc times out.
typedef void (*ec_poll_fn)(void *user_data);

/// Optional callback to provide a dynamic prompt. If set,
/// called before each readline instead of using the static
/// prompt string.
typedef const char *(*ec_prompt_fn)(void *user_data);

typedef struct {
  const char *product_name;
  const char *version;
  const char *prompt;
  ec_command_tree tree;
  ec_role_gate caller_role;
  ec_dispatch_fn dispatch;
  ec_poll_fn poll;
  ec_prompt_fn get_prompt;
  void *user_data;
  const ec_io *io;
} ec_shell;

/// Run the REPL until EOF or `exit`. Blocking.
/// Auto-detects JSON input (lines starting with '{') and
/// responds in JSON. Interactive lines get the normal CLI.
void ec_shell_run(ec_shell *shell);

/// Dump the command tree as a JSON schema. The controlling
/// device can use this to validate commands before sending.
/// Format: {"commands":[{"path":"...","args":[...],...},...]}
size_t ec_ct_schema_json(const ec_command_tree *tree,
                         ec_buf *buf);

#ifdef __cplusplus
}
#endif

#endif  // EINHEIT_CORE_EC_H_
