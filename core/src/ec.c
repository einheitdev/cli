/// @file ec.c
/// @brief einheit_core implementation — all modules in one file.
// Copyright (c) 2026 Einheit Networks

#include "einheit/core/ec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ── Output buffer ───────────────────────────────────────────

size_t ec_buf_append(ec_buf *buf, const char *src, size_t n) {
  if (!buf || !buf->data || buf->cap == 0) return 0;
  size_t avail = buf->cap - buf->used - 1;
  size_t w = n < avail ? n : avail;
  if (w > 0) {
    memcpy(buf->data + buf->used, src, w);
    buf->used += w;
  }
  buf->data[buf->used] = '\0';
  return w;
}

size_t ec_buf_puts(ec_buf *buf, const char *s) {
  if (!s) return 0;
  return ec_buf_append(buf, s, strlen(s));
}

size_t ec_buf_printf(ec_buf *buf, const char *fmt, ...) {
  if (!buf || !buf->data || buf->cap == 0) return 0;
  size_t avail = buf->cap - buf->used;
  if (avail <= 1) return 0;
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf->data + buf->used, avail, fmt, ap);
  va_end(ap);
  if (n < 0) return 0;
  size_t written = (size_t)n < avail - 1 ? (size_t)n : avail - 1;
  buf->used += written;
  return written;
}

void ec_buf_clear(ec_buf *buf) {
  if (!buf || !buf->data) return;
  buf->used = 0;
  buf->data[0] = '\0';
}

// ── Errors ──────────────────────────────────────────────────

void ec_error_set(ec_error *err, ec_error_code code,
                  const char *fmt, ...) {
  if (!err) return;
  err->code = code;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(err->message, sizeof(err->message), fmt, ap);
  va_end(ap);
}

// ── Roles ───────────────────────────────────────────────────

const char *ec_role_label(ec_role_gate role) {
  switch (role) {
    case EC_ROLE_ADMIN:    return "admin";
    case EC_ROLE_OPERATOR: return "operator";
    default:               return "any";
  }
}

bool ec_role_allows(ec_role_gate caller, ec_role_gate required) {
  return (int)caller >= (int)required;
}

// ── Fuzzy matching ──────────────────────────────────────────

size_t ec_fuzzy_distance(const char *a, size_t la,
                         const char *b, size_t lb) {
  if (la == 0) return lb;
  if (lb == 0) return la;
  // Two-row DP on the stack.
  size_t prev[EC_MAX_TOKEN + 1];
  size_t cur[EC_MAX_TOKEN + 1];
  if (lb > EC_MAX_TOKEN) lb = EC_MAX_TOKEN;
  for (size_t j = 0; j <= lb; ++j) prev[j] = j;
  for (size_t i = 1; i <= la; ++i) {
    cur[0] = i;
    for (size_t j = 1; j <= lb; ++j) {
      size_t cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
      size_t ins = cur[j - 1] + 1;
      size_t del = prev[j] + 1;
      size_t sub = prev[j - 1] + cost;
      size_t best = ins;
      if (del < best) best = del;
      if (sub < best) best = sub;
      cur[j] = best;
    }
    memcpy(prev, cur, (lb + 1) * sizeof(size_t));
  }
  return prev[lb];
}

static size_t fuzzy_threshold(size_t len) {
  if (len <= 4) return 1;
  if (len <= 8) return 2;
  return 3;
}

typedef struct {
  int prefix_score;
  size_t distance;
  const char *word;
} fuzzy_entry;

static int fuzzy_cmp(const void *va, const void *vb) {
  const fuzzy_entry *a = (const fuzzy_entry *)va;
  const fuzzy_entry *b = (const fuzzy_entry *)vb;
  if (a->prefix_score != b->prefix_score)
    return a->prefix_score - b->prefix_score;
  if (a->distance != b->distance)
    return (a->distance < b->distance) ? -1 : 1;
  return strcmp(a->word, b->word);
}

size_t ec_fuzzy_suggest(const char *query,
                        const char *const *vocab,
                        size_t vocab_count,
                        char out[][EC_MAX_TOKEN],
                        size_t out_cap) {
  size_t qlen = strlen(query);
  size_t threshold = fuzzy_threshold(qlen);
  fuzzy_entry scored[EC_MAX_COMPLETIONS];
  size_t sc = 0;

  for (size_t v = 0; v < vocab_count; ++v) {
    const char *w = vocab[v];
    if (strcmp(w, query) == 0) continue;
    size_t wlen = strlen(w);
    bool is_prefix = qlen > 0 && wlen >= qlen &&
                     memcmp(w, query, qlen) == 0;
    size_t d = ec_fuzzy_distance(query, qlen, w, wlen);
    if (is_prefix && sc < EC_MAX_COMPLETIONS) {
      scored[sc++] = (fuzzy_entry){0, d, w};
    } else if (d <= threshold && sc < EC_MAX_COMPLETIONS) {
      scored[sc++] = (fuzzy_entry){1, d, w};
    }
  }
  qsort(scored, sc, sizeof(scored[0]), fuzzy_cmp);
  size_t written = 0;
  for (size_t i = 0; i < sc; ++i) {
    if (written < out_cap) {
      snprintf(out[written], EC_MAX_TOKEN, "%s", scored[i].word);
    }
    written++;
  }
  return written;
}

// ── Command tree ────────────────────────────────────────────

static void split_path(ec_command_spec *spec) {
  spec->path_token_count = 0;
  const char *p = spec->path;
  while (*p && spec->path_token_count < EC_MAX_PATH_TOKENS) {
    while (*p == ' ') ++p;
    if (!*p) break;
    const char *start = p;
    while (*p && *p != ' ') ++p;
    size_t len = (size_t)(p - start);
    if (len >= EC_MAX_TOKEN) len = EC_MAX_TOKEN - 1;
    memcpy(spec->path_tokens[spec->path_token_count],
           start, len);
    spec->path_tokens[spec->path_token_count][len] = '\0';
    spec->path_token_count++;
  }
}

void ec_ct_init(ec_command_tree *tree) {
  memset(tree, 0, sizeof(*tree));
}

ec_error_code ec_ct_register(ec_command_tree *tree,
                             const ec_command_spec *spec) {
  if (tree->count >= EC_MAX_COMMANDS)
    return EC_ERR_DUPLICATE_REGISTRATION;
  for (size_t i = 0; i < tree->count; ++i) {
    if (strcmp(tree->specs[i].path, spec->path) == 0)
      return EC_ERR_DUPLICATE_REGISTRATION;
  }
  tree->specs[tree->count] = *spec;
  split_path(&tree->specs[tree->count]);
  tree->count++;
  return EC_OK;
}

ec_error_code ec_ct_parse(const ec_command_tree *tree,
                          const char *const *tokens,
                          size_t token_count,
                          ec_role_gate caller_role,
                          ec_parsed_command *out,
                          ec_error *err) {
  memset(out, 0, sizeof(*out));

  const ec_command_spec *best = NULL;
  size_t best_len = 0;
  const ec_command_spec *ambiguous[EC_MAX_COMMANDS];
  size_t ambig_count = 0;
  size_t max_len = 0;

  for (size_t s = 0; s < tree->count; ++s) {
    const ec_command_spec *spec = &tree->specs[s];
    if (spec->path_token_count > token_count) continue;
    bool ok = true;
    bool all_exact = true;
    for (size_t i = 0; i < spec->path_token_count; ++i) {
      const char *pt = spec->path_tokens[i];
      const char *tk = tokens[i];
      if (strcmp(pt, tk) == 0) continue;
      size_t tl = strlen(tk);
      if (strlen(pt) >= tl && memcmp(pt, tk, tl) == 0) {
        all_exact = false;
        continue;
      }
      ok = false;
      break;
    }
    if (!ok) continue;
    if (spec->path_token_count > max_len) {
      max_len = spec->path_token_count;
      best = all_exact ? spec : NULL;
      ambig_count = all_exact ? 0 : 1;
      if (!all_exact) ambiguous[0] = spec;
    } else if (spec->path_token_count == max_len) {
      if (all_exact && !best) {
        best = spec;
      } else if (!all_exact) {
        if (ambig_count < EC_MAX_COMMANDS)
          ambiguous[ambig_count++] = spec;
      }
    }
  }

  if (!best) {
    if (ambig_count == 1) {
      best = ambiguous[0];
      best_len = max_len;
    } else if (ambig_count > 1) {
      char opts[256];
      ec_buf ob = {opts, sizeof(opts), 0};
      for (size_t i = 0; i < ambig_count; ++i) {
        if (i > 0) ec_buf_puts(&ob, ", ");
        ec_buf_puts(&ob, ambiguous[i]->path);
      }
      ec_error_set(err, EC_ERR_UNKNOWN_COMMAND,
                   "ambiguous: %s", opts);
      return EC_ERR_UNKNOWN_COMMAND;
    }
  } else {
    best_len = max_len;
  }

  if (!best) {
    if (token_count > 0) {
      const char *verbs[EC_MAX_COMMANDS];
      size_t vc = 0;
      for (size_t i = 0; i < tree->count; ++i) {
        if (tree->specs[i].path_token_count == 0) continue;
        const char *v = tree->specs[i].path_tokens[0];
        bool dup = false;
        for (size_t j = 0; j < vc; ++j) {
          if (strcmp(verbs[j], v) == 0) { dup = true; break; }
        }
        if (!dup && vc < EC_MAX_COMMANDS) verbs[vc++] = v;
      }
      char hints[4][EC_MAX_TOKEN];
      size_t nh = ec_fuzzy_suggest(tokens[0], verbs, vc,
                                   hints, 4);
      if (nh > 0)
        ec_error_set(err, EC_ERR_UNKNOWN_COMMAND,
                     "unknown command — did you mean '%s'?",
                     hints[0]);
      else
        ec_error_set(err, EC_ERR_UNKNOWN_COMMAND,
                     "unknown command");
    } else {
      ec_error_set(err, EC_ERR_UNKNOWN_COMMAND,
                   "unknown command");
    }
    return EC_ERR_UNKNOWN_COMMAND;
  }

  if (!ec_role_allows(caller_role, best->role)) {
    ec_error_set(err, EC_ERR_NOT_AUTHORISED,
                 "insufficient privileges");
    return EC_ERR_NOT_AUTHORISED;
  }

  out->spec = best;
  for (size_t i = best_len; i < token_count; ++i) {
    if (out->arg_count < EC_MAX_ARGS) {
      snprintf(out->args[out->arg_count], EC_MAX_TOKEN,
               "%s", tokens[i]);
      out->arg_count++;
    }
  }
  for (size_t i = 0; i < best->arg_count; ++i) {
    if (i >= out->arg_count && best->args[i].required) {
      ec_error_set(err, EC_ERR_MISSING_ARGUMENT,
                   "missing: %s", best->args[i].name);
      return EC_ERR_MISSING_ARGUMENT;
    }
  }
  return EC_OK;
}

size_t ec_ct_suggest(const ec_command_tree *tree,
                     const char *const *preceding,
                     size_t prec_count, const char *partial,
                     char out[][EC_MAX_TOKEN],
                     size_t out_cap) {
  size_t written = 0;
  size_t plen = strlen(partial);
  for (size_t s = 0; s < tree->count; ++s) {
    const ec_command_spec *spec = &tree->specs[s];
    if (spec->path_token_count <= prec_count) continue;
    bool match = true;
    for (size_t i = 0; i < prec_count; ++i) {
      if (strcmp(spec->path_tokens[i], preceding[i]) != 0) {
        match = false;
        break;
      }
    }
    if (!match) continue;
    const char *next = spec->path_tokens[prec_count];
    if (plen > 0 && strncmp(next, partial, plen) != 0)
      continue;
    bool dup = false;
    for (size_t i = 0; i < written && i < out_cap; ++i) {
      if (strcmp(out[i], next) == 0) { dup = true; break; }
    }
    if (dup) continue;
    if (written < out_cap)
      snprintf(out[written], EC_MAX_TOKEN, "%s", next);
    written++;
  }
  // Bubble sort — tiny arrays.
  for (size_t i = 0; i + 1 < written && i + 1 < out_cap; ++i) {
    for (size_t j = i + 1; j < written && j < out_cap; ++j) {
      if (strcmp(out[i], out[j]) > 0) {
        char tmp[EC_MAX_TOKEN];
        memcpy(tmp, out[i], EC_MAX_TOKEN);
        memcpy(out[i], out[j], EC_MAX_TOKEN);
        memcpy(out[j], tmp, EC_MAX_TOKEN);
      }
    }
  }
  return written;
}

static int spec_cmp(const void *a, const void *b) {
  return strcmp((*(const ec_command_spec **)a)->path,
               (*(const ec_command_spec **)b)->path);
}

size_t ec_ct_format_help(const ec_command_tree *tree,
                         ec_buf *buf) {
  const ec_command_spec *sorted[EC_MAX_COMMANDS];
  for (size_t i = 0; i < tree->count; ++i)
    sorted[i] = &tree->specs[i];
  qsort(sorted, tree->count, sizeof(sorted[0]), spec_cmp);
  size_t w = 0;
  for (size_t i = 0; i < tree->count; ++i) {
    size_t l = strlen(sorted[i]->path);
    if (l > w) w = l;
  }
  size_t start = buf->used;
  for (size_t i = 0; i < tree->count; ++i)
    ec_buf_printf(buf, "  %-*s  %s\n", (int)w,
                  sorted[i]->path, sorted[i]->help);
  return buf->used - start;
}

size_t ec_ct_format_command(const ec_command_spec *spec,
                            ec_buf *buf) {
  size_t start = buf->used;
  ec_buf_printf(buf, "%s\n", spec->path);
  if (spec->help[0])
    ec_buf_printf(buf, "  %s\n", spec->help);
  ec_buf_printf(buf, "  role: %s\n", ec_role_label(spec->role));
  for (size_t i = 0; i < spec->arg_count; ++i)
    ec_buf_printf(buf, "  <%s> %s %s\n", spec->args[i].name,
                  spec->args[i].required ? "(required)"
                                         : "(optional)",
                  spec->args[i].help);
  return buf->used - start;
}

// ── Semantic table ──────────────────────────────────────────

void ec_table_init(ec_table *t) {
  memset(t, 0, sizeof(*t));
}

void ec_table_add_column(ec_table *t, const char *header,
                         ec_align align) {
  if (t->column_count >= EC_TABLE_MAX_COLUMNS) return;
  ec_column *c = &t->columns[t->column_count++];
  snprintf(c->header, EC_TABLE_MAX_TEXT, "%s", header);
  c->align = align;
}

void ec_table_add_row(ec_table *t, const ec_cell *cells,
                      size_t count) {
  if (t->row_count >= EC_TABLE_MAX_ROWS) return;
  size_t r = t->row_count++;
  size_t n = count < t->column_count ? count : t->column_count;
  for (size_t i = 0; i < n; ++i) t->rows[r][i] = cells[i];
}

ec_cell ec_cell_make(const char *text, ec_semantic sem) {
  ec_cell c;
  memset(&c, 0, sizeof(c));
  if (text) snprintf(c.text, EC_TABLE_MAX_TEXT, "%s", text);
  c.semantic = sem;
  return c;
}

const char *ec_plain_marker(ec_semantic s) {
  switch (s) {
    case EC_SEM_GOOD: return "[OK]";
    case EC_SEM_WARN: return "[!!]";
    case EC_SEM_BAD:  return "[FAIL]";
    case EC_SEM_DIM:  return "[--]";
    default:          return "";
  }
}

size_t ec_table_render(const ec_table *t, ec_buf *buf) {
  if (t->column_count == 0) return 0;
  size_t start = buf->used;

  // Compute column widths.
  size_t widths[EC_TABLE_MAX_COLUMNS];
  for (size_t c = 0; c < t->column_count; ++c)
    widths[c] = strlen(t->columns[c].header);
  for (size_t r = 0; r < t->row_count; ++r) {
    for (size_t c = 0; c < t->column_count; ++c) {
      const char *marker = ec_plain_marker(
          t->rows[r][c].semantic);
      size_t w = strlen(t->rows[r][c].text);
      if (marker[0]) w += strlen(marker) + 1;
      if (w > widths[c]) widths[c] = w;
    }
  }

  // Header.
  for (size_t c = 0; c < t->column_count; ++c) {
    if (c > 0) ec_buf_puts(buf, " | ");
    ec_buf_printf(buf, "%-*s", (int)widths[c],
                  t->columns[c].header);
  }
  ec_buf_puts(buf, "\n");

  // Separator.
  for (size_t c = 0; c < t->column_count; ++c) {
    if (c > 0) ec_buf_puts(buf, "-+-");
    for (size_t i = 0; i < widths[c]; ++i)
      ec_buf_puts(buf, "-");
  }
  ec_buf_puts(buf, "\n");

  // Rows.
  for (size_t r = 0; r < t->row_count; ++r) {
    for (size_t c = 0; c < t->column_count; ++c) {
      if (c > 0) ec_buf_puts(buf, " | ");
      const ec_cell *cell = &t->rows[r][c];
      const char *marker = ec_plain_marker(cell->semantic);
      char display[EC_TABLE_MAX_TEXT + 16];
      if (marker[0])
        snprintf(display, sizeof(display), "%s %s",
                 marker, cell->text);
      else
        snprintf(display, sizeof(display), "%s", cell->text);
      if (t->columns[c].align == EC_ALIGN_RIGHT)
        ec_buf_printf(buf, "%*s", (int)widths[c], display);
      else
        ec_buf_printf(buf, "%-*s", (int)widths[c], display);
    }
    ec_buf_puts(buf, "\n");
  }
  return buf->used - start;
}

// ── JSON table renderer ─────────────────────────────────────

static void json_escape(const char *s, ec_buf *buf) {
  for (const char *p = s; *p; ++p) {
    switch (*p) {
      case '"':  ec_buf_puts(buf, "\\\""); break;
      case '\\': ec_buf_puts(buf, "\\\\"); break;
      case '\n': ec_buf_puts(buf, "\\n"); break;
      case '\r': ec_buf_puts(buf, "\\r"); break;
      case '\t': ec_buf_puts(buf, "\\t"); break;
      default:   ec_buf_append(buf, p, 1);
    }
  }
}

size_t ec_table_render_json(const ec_table *t, ec_buf *buf) {
  size_t start = buf->used;
  ec_buf_puts(buf, "[");
  for (size_t r = 0; r < t->row_count; ++r) {
    if (r > 0) ec_buf_puts(buf, ",");
    ec_buf_puts(buf, "{");
    for (size_t c = 0; c < t->column_count; ++c) {
      if (c > 0) ec_buf_puts(buf, ",");
      ec_buf_puts(buf, "\"");
      json_escape(t->columns[c].header, buf);
      ec_buf_puts(buf, "\":\"");
      json_escape(t->rows[r][c].text, buf);
      ec_buf_puts(buf, "\"");
    }
    ec_buf_puts(buf, "}");
  }
  ec_buf_puts(buf, "]");
  return buf->used - start;
}

// ── JSON command schema ─────────────────────────────────────

size_t ec_ct_schema_json(const ec_command_tree *tree,
                         ec_buf *buf) {
  size_t start = buf->used;
  ec_buf_puts(buf, "{\"commands\":[");
  for (size_t i = 0; i < tree->count; ++i) {
    const ec_command_spec *s = &tree->specs[i];
    if (i > 0) ec_buf_puts(buf, ",");
    ec_buf_puts(buf, "{\"path\":\"");
    json_escape(s->path, buf);
    ec_buf_puts(buf, "\",\"help\":\"");
    json_escape(s->help, buf);
    ec_buf_printf(buf, "\",\"role\":\"%s\"",
                  ec_role_label(s->role));
    if (s->arg_count > 0) {
      ec_buf_puts(buf, ",\"args\":[");
      for (size_t a = 0; a < s->arg_count; ++a) {
        if (a > 0) ec_buf_puts(buf, ",");
        ec_buf_puts(buf, "{\"name\":\"");
        json_escape(s->args[a].name, buf);
        ec_buf_puts(buf, "\",\"help\":\"");
        json_escape(s->args[a].help, buf);
        ec_buf_printf(buf, "\",\"required\":%s}",
                      s->args[a].required ? "true" : "false");
      }
      ec_buf_puts(buf, "]");
    }
    ec_buf_puts(buf, "}");
  }
  ec_buf_puts(buf, "]}");
  return buf->used - start;
}

// ── Line reader ─────────────────────────────────────────────

void ec_history_init(ec_history *h) {
  memset(h, 0, sizeof(*h));
}

void ec_history_add(ec_history *h, const char *line) {
  if (!line || !line[0]) return;
  size_t slot = (h->oldest + h->count) % EC_HISTORY_SIZE;
  if (h->count < EC_HISTORY_SIZE) {
    h->count++;
  } else {
    slot = h->oldest;
    h->oldest = (h->oldest + 1) % EC_HISTORY_SIZE;
  }
  snprintf(h->lines[slot], EC_LINE_MAX, "%s", line);
}

static void tokenize(const char *line, const char **tokens,
                      size_t *count, size_t max_tokens) {
  *count = 0;
  static char scratch[EC_LINE_MAX];
  snprintf(scratch, EC_LINE_MAX, "%s", line);
  char *p = scratch;
  while (*p && *count < max_tokens) {
    while (*p == ' ') ++p;
    if (!*p) break;
    tokens[(*count)++] = p;
    while (*p && *p != ' ') ++p;
    if (*p) *p++ = '\0';
  }
}

static ec_poll_fn ec_shell_poll_fn_ = NULL;
static void *ec_shell_poll_ctx_ = NULL;

int ec_readline(const ec_io *io, const char *prompt,
                const ec_command_tree *tree,
                ec_history *hist,
                char *buf, size_t buf_size) {
  io->puts(prompt);
  size_t pos = 0;
  int hist_idx = hist ? (int)hist->count : 0;

  while (1) {
    int ch = io->getc();
    if (ch == -1) return -1;
    if (ch < 0) {
      if (ec_shell_poll_fn_ && ec_shell_poll_ctx_)
        ec_shell_poll_fn_(ec_shell_poll_ctx_);
      continue;
    }

    if (ch == '\r' || ch == '\n') {
      io->puts("\r\n");
      buf[pos] = '\0';
      return (int)pos;
    }

    // Backspace / DEL.
    if (ch == 0x08 || ch == 0x7F) {
      if (pos > 0) {
        pos--;
        io->puts("\b \b");
      }
      continue;
    }

    // Tab — completion.
    if (ch == '\t' && tree) {
      buf[pos] = '\0';
      const char *tokens[EC_MAX_PATH_TOKENS + EC_MAX_ARGS];
      size_t tc = 0;
      tokenize(buf, tokens, &tc, EC_MAX_PATH_TOKENS + EC_MAX_ARGS);

      const char *partial = "";
      size_t prec = tc;
      bool trailing_space = (pos > 0 && buf[pos - 1] == ' ');
      if (tc > 0 && !trailing_space) {
        partial = tokens[tc - 1];
        prec = tc - 1;
      }
      char completions[EC_MAX_COMPLETIONS][EC_MAX_TOKEN];
      size_t nc = ec_ct_suggest(tree, tokens, prec, partial,
                                completions, EC_MAX_COMPLETIONS);
      if (nc == 1) {
        // Complete the word.
        size_t plen = strlen(partial);
        size_t clen = strlen(completions[0]);
        for (size_t i = plen; i < clen && pos < buf_size - 2;
             ++i) {
          buf[pos++] = completions[0][i];
          char s[2] = {completions[0][i], '\0'};
          io->puts(s);
        }
        buf[pos++] = ' ';
        io->puts(" ");
      } else if (nc > 1) {
        io->puts("\r\n");
        for (size_t i = 0; i < nc && i < EC_MAX_COMPLETIONS;
             ++i) {
          io->puts("  ");
          io->puts(completions[i]);
          io->puts("\r\n");
        }
        buf[pos] = '\0';
        io->puts(prompt);
        io->puts(buf);
      }
      continue;
    }

    // ? at start of line or after space — help.
    if (ch == '?' && tree) {
      io->puts("\r\n");
      char help_buf[2048];
      ec_buf hb = {help_buf, sizeof(help_buf), 0};
      ec_ct_format_help(tree, &hb);
      io->puts(help_buf);
      buf[pos] = '\0';
      io->puts(prompt);
      io->puts(buf);
      continue;
    }

    // Escape sequences (arrow keys).
    if (ch == 0x1B) {
      int c2 = io->getc();
      if (c2 != '[') continue;
      int c3 = io->getc();
      if (c3 == 'A' && hist) {
        // Up arrow.
        if (hist_idx > 0) {
          hist_idx--;
          size_t slot =
              (hist->oldest + (size_t)hist_idx) % EC_HISTORY_SIZE;
          // Clear line.
          while (pos > 0) { io->puts("\b \b"); pos--; }
          snprintf(buf, buf_size, "%s", hist->lines[slot]);
          pos = strlen(buf);
          io->puts(buf);
        }
      } else if (c3 == 'B' && hist) {
        // Down arrow.
        if (hist_idx < (int)hist->count - 1) {
          hist_idx++;
          size_t slot =
              (hist->oldest + (size_t)hist_idx) % EC_HISTORY_SIZE;
          while (pos > 0) { io->puts("\b \b"); pos--; }
          snprintf(buf, buf_size, "%s", hist->lines[slot]);
          pos = strlen(buf);
          io->puts(buf);
        } else {
          hist_idx = (int)hist->count;
          while (pos > 0) { io->puts("\b \b"); pos--; }
          buf[0] = '\0';
        }
      }
      continue;
    }

    // Ctrl-C — clear line.
    if (ch == 0x03) {
      io->puts("^C\r\n");
      pos = 0;
      buf[0] = '\0';
      io->puts(prompt);
      continue;
    }

    // Ctrl-D on empty line — EOF.
    if (ch == 0x04 && pos == 0) return -1;

    // Regular character.
    if (ch >= 0x20 && ch < 0x7F && pos < buf_size - 1) {
      buf[pos++] = (char)ch;
      char s[2] = {(char)ch, '\0'};
      io->puts(s);
    }
  }
}

// ── Minimal JSON field extractor ─────────────────────────────
// Extracts a string value for a given key from a flat JSON
// object. No nesting, no arrays — just {"key":"value",...}.
// Returns length of value, or 0 if not found.

static size_t json_get_str(const char *json, const char *key,
                           char *out, size_t out_size) {
  size_t klen = strlen(key);
  const char *p = json;
  while ((p = strstr(p, "\"")) != NULL) {
    p++;
    if (strncmp(p, key, klen) == 0 && p[klen] == '"') {
      p += klen + 1;
      // Skip to colon, then opening quote.
      while (*p && *p != ':') p++;
      if (!*p) return 0;
      p++;
      while (*p == ' ') p++;
      if (*p != '"') return 0;
      p++;
      size_t i = 0;
      while (*p && *p != '"' && i < out_size - 1) {
        if (*p == '\\' && p[1]) { p++; }
        out[i++] = *p++;
      }
      out[i] = '\0';
      return i;
    }
  }
  out[0] = '\0';
  return 0;
}

// Extract a JSON array of strings: "key":["a","b","c"]
// Returns number of elements extracted.
static size_t json_get_arr(const char *json, const char *key,
                           char out[][EC_MAX_TOKEN],
                           size_t max_items) {
  size_t klen = strlen(key);
  const char *p = json;
  while ((p = strstr(p, "\"")) != NULL) {
    p++;
    if (strncmp(p, key, klen) == 0 && p[klen] == '"') {
      p += klen + 1;
      while (*p && *p != '[') p++;
      if (!*p) return 0;
      p++;
      size_t count = 0;
      while (*p && *p != ']' && count < max_items) {
        while (*p == ' ' || *p == ',') p++;
        if (*p == '"') {
          p++;
          size_t i = 0;
          while (*p && *p != '"' && i < EC_MAX_TOKEN - 1) {
            if (*p == '\\' && p[1]) { p++; }
            out[count][i++] = *p++;
          }
          out[count][i] = '\0';
          if (*p == '"') p++;
          count++;
        } else {
          break;
        }
      }
      return count;
    }
  }
  return 0;
}

static int json_get_int(const char *json, const char *key) {
  size_t klen = strlen(key);
  const char *p = json;
  while ((p = strstr(p, "\"")) != NULL) {
    p++;
    if (strncmp(p, key, klen) == 0 && p[klen] == '"') {
      p += klen + 1;
      while (*p && *p != ':') p++;
      if (!*p) return -1;
      p++;
      while (*p == ' ') p++;
      return atoi(p);
    }
  }
  return -1;
}

// ── Output capture for JSON dispatch ────────────────────────

static ec_buf *shell_capture_buf_ = NULL;
static void capture_puts_(const char *s) {
  if (shell_capture_buf_)
    ec_buf_puts(shell_capture_buf_, s);
}

// ── JSON dispatch ───────────────────────────────────────────
// Input:  {"cmd":"show interfaces","args":["ge1"],"id":1}
// Output: {"id":1,"ok":true,"data":[...]} or
//         {"id":1,"ok":false,"error":"..."}
// Special: {"cmd":"schema","id":0} returns the command schema.

static void shell_handle_json(ec_shell *shell,
                              const char *line) {
  char cmd[80] = "";
  char args[EC_MAX_ARGS][EC_MAX_TOKEN];
  int id = json_get_int(line, "id");
  json_get_str(line, "cmd", cmd, sizeof(cmd));
  size_t argc = json_get_arr(line, "args", args, EC_MAX_ARGS);

  char resp[4096];
  ec_buf rb = {resp, sizeof(resp), 0};

  // Schema request.
  if (strcmp(cmd, "schema") == 0) {
    ec_buf_printf(&rb, "{\"id\":%d,\"ok\":true,\"data\":", id);
    ec_ct_schema_json(&shell->tree, &rb);
    ec_buf_puts(&rb, "}\n");
    shell->io->puts(resp);
    return;
  }

  // Build token array: split cmd into path tokens, append args.
  const char *tokens[EC_MAX_PATH_TOKENS + EC_MAX_ARGS];
  size_t tc = 0;
  static char cmd_tokens[EC_MAX_PATH_TOKENS][EC_MAX_TOKEN];
  char cmd_copy[80];
  snprintf(cmd_copy, sizeof(cmd_copy), "%s", cmd);
  char *sp = cmd_copy;
  while (*sp && tc < EC_MAX_PATH_TOKENS) {
    while (*sp == ' ') sp++;
    if (!*sp) break;
    char *start = sp;
    while (*sp && *sp != ' ') sp++;
    size_t len = (size_t)(sp - start);
    if (len >= EC_MAX_TOKEN) len = EC_MAX_TOKEN - 1;
    memcpy(cmd_tokens[tc], start, len);
    cmd_tokens[tc][len] = '\0';
    tokens[tc] = cmd_tokens[tc];
    tc++;
    if (*sp) sp++;
  }
  for (size_t i = 0; i < argc && tc < EC_MAX_PATH_TOKENS + EC_MAX_ARGS; ++i) {
    tokens[tc++] = args[i];
  }

  if (tc == 0) {
    ec_buf_printf(&rb,
                  "{\"id\":%d,\"ok\":false,"
                  "\"error\":\"empty command\"}\n", id);
    shell->io->puts(resp);
    return;
  }

  // Parse.
  ec_parsed_command pc;
  ec_error err;
  ec_error_code rc = ec_ct_parse(
      &shell->tree, tokens, tc,
      shell->caller_role, &pc, &err);
  if (rc != EC_OK) {
    ec_buf_printf(&rb, "{\"id\":%d,\"ok\":false,\"error\":\"",
                  id);
    json_escape(err.message, &rb);
    ec_buf_puts(&rb, "\"}\n");
    shell->io->puts(resp);
    return;
  }

  // Dispatch — capture output into a buffer.
  char output[3072];
  ec_buf ob = {output, sizeof(output), 0};
  shell_capture_buf_ = &ob;
  ec_io json_io = {
      .puts = capture_puts_,
      .getc = NULL,
  };

  rc = shell->dispatch(&pc, &json_io, shell->user_data);
  shell_capture_buf_ = NULL;

  ec_buf_printf(&rb, "{\"id\":%d,\"ok\":%s",
                id, (rc == EC_OK || rc == EC_MODE_CHANGED)
                    ? "true" : "false");
  if (ob.used > 0) {
    ec_buf_puts(&rb, ",\"output\":\"");
    json_escape(output, &rb);
    ec_buf_puts(&rb, "\"");
  }
  ec_buf_puts(&rb, "}\n");
  shell->io->puts(resp);
}

// ── Shell ───────────────────────────────────────────────────

void ec_shell_run(ec_shell *shell) {
  ec_shell_poll_fn_ = shell->poll;
  ec_shell_poll_ctx_ = shell->user_data;

  ec_history hist;
  ec_history_init(&hist);

  // Banner.
  shell->io->puts("\r\n  ");
  shell->io->puts(shell->product_name);
  shell->io->puts(" ");
  shell->io->puts(shell->version);
  shell->io->puts("\r\n\r\n");

  char line[EC_LINE_MAX];
  while (1) {
    const char *prompt = shell->get_prompt
        ? shell->get_prompt(shell->user_data)
        : shell->prompt;
    int len = ec_readline(shell->io, prompt,
                          &shell->tree, &hist,
                          line, sizeof(line));
    if (len < 0) break;
    if (len == 0) continue;

    // JSON mode: line starts with '{'.
    if (line[0] == '{') {
      shell_handle_json(shell, line);
      continue;
    }

    ec_history_add(&hist, line);

    // Tokenize.
    const char *tokens[EC_MAX_PATH_TOKENS + EC_MAX_ARGS];
    size_t tc = 0;
    tokenize(line, tokens, &tc,
             EC_MAX_PATH_TOKENS + EC_MAX_ARGS);
    if (tc == 0) continue;

    // Built-in: exit / quit. Try dispatch first (configure
    // mode exit), fall back to shell exit.
    if (strcmp(tokens[0], "exit") == 0 ||
        strcmp(tokens[0], "quit") == 0) {
      ec_parsed_command pc;
      ec_error err;
      ec_error_code rc = ec_ct_parse(
          &shell->tree, tokens, tc,
          shell->caller_role, &pc, &err);
      if (rc == EC_OK) {
        rc = shell->dispatch(&pc, shell->io,
                             shell->user_data);
        if (rc == EC_MODE_CHANGED) continue;
      }
      break;
    }

    // Built-in: help.
    if (strcmp(tokens[0], "help") == 0) {
      if (tc == 1) {
        char help_buf[2048];
        ec_buf hb = {help_buf, sizeof(help_buf), 0};
        ec_ct_format_help(&shell->tree, &hb);
        shell->io->puts(help_buf);
      } else {
        for (size_t i = 0; i < shell->tree.count; ++i) {
          if (strcmp(shell->tree.specs[i].path,
                    tokens[1]) == 0) {
            char help_buf[512];
            ec_buf hb = {help_buf, sizeof(help_buf), 0};
            ec_ct_format_command(&shell->tree.specs[i], &hb);
            shell->io->puts(help_buf);
            break;
          }
        }
      }
      continue;
    }

    // Parse and dispatch.
    ec_parsed_command pc;
    ec_error err;
    ec_error_code rc = ec_ct_parse(
        &shell->tree, tokens, tc,
        shell->caller_role, &pc, &err);
    if (rc != EC_OK) {
      shell->io->puts("[FAIL] ");
      shell->io->puts(err.message);
      shell->io->puts("\r\n");
      continue;
    }

    rc = shell->dispatch(&pc, shell->io, shell->user_data);
    if (rc != EC_OK && rc != EC_MODE_CHANGED) {
      shell->io->puts("[FAIL] command failed\r\n");
    }
  }
}
