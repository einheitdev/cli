/// @file test_ec.c
/// @brief Tests for the embedded CLI core.
// Copyright (c) 2026 Einheit Networks

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "einheit/core/ec.h"

static int failures = 0;
static int tests = 0;

#define RUN(name) do {                                \
  tests++;                                            \
  printf("[ RUN      ] %s\n", #name);                \
  test_##name();                                      \
  printf("[       OK ] %s\n", #name);                 \
} while (0)

#define EXPECT_EQ(a, b) do {                          \
  if ((a) != (b)) {                                   \
    printf("  FAIL %s:%d: %s != %s\n",               \
           __FILE__, __LINE__, #a, #b);               \
    failures++;                                       \
  }                                                   \
} while (0)

#define EXPECT_TRUE(e) do {                           \
  if (!(e)) {                                         \
    printf("  FAIL %s:%d: %s\n",                     \
           __FILE__, __LINE__, #e);                   \
    failures++;                                       \
  }                                                   \
} while (0)

#define EXPECT_STREQ(a, b) do {                       \
  if (strcmp((a), (b)) != 0) {                        \
    printf("  FAIL %s:%d: \"%s\" != \"%s\"\n",       \
           __FILE__, __LINE__, (a), (b));             \
    failures++;                                       \
  }                                                   \
} while (0)

// ── buf ─────────────────────────────────────────────────────

static void test_buf_basic(void) {
  char s[32];
  ec_buf b = {s, sizeof(s), 0};
  ec_buf_puts(&b, "hello");
  ec_buf_puts(&b, " world");
  EXPECT_STREQ(s, "hello world");
  EXPECT_EQ(b.used, 11u);
}

static void test_buf_truncate(void) {
  char s[8];
  ec_buf b = {s, sizeof(s), 0};
  ec_buf_puts(&b, "hello world");
  EXPECT_STREQ(s, "hello w");
}

// ── fuzzy ───────────────────────────────────────────────────

static void test_distance(void) {
  EXPECT_EQ(ec_fuzzy_distance("show", 4, "show", 4), 0u);
  EXPECT_EQ(ec_fuzzy_distance("show", 4, "shw", 3), 1u);
  EXPECT_EQ(ec_fuzzy_distance("kitten", 6, "sitting", 7), 3u);
}

static void test_suggest(void) {
  const char *vocab[] = {"show", "set", "shell"};
  char out[4][EC_MAX_TOKEN];
  size_t n = ec_fuzzy_suggest("shw", vocab, 3, out, 4);
  EXPECT_TRUE(n >= 1);
  EXPECT_STREQ(out[0], "show");
}

// ── roles ───────────────────────────────────────────────────

static void test_roles(void) {
  EXPECT_TRUE(ec_role_allows(EC_ROLE_ADMIN, EC_ROLE_ADMIN));
  EXPECT_TRUE(ec_role_allows(EC_ROLE_ADMIN, EC_ROLE_OPERATOR));
  EXPECT_TRUE(ec_role_allows(EC_ROLE_ADMIN, EC_ROLE_ANY));
  EXPECT_TRUE(ec_role_allows(EC_ROLE_OPERATOR, EC_ROLE_OPERATOR));
  EXPECT_TRUE(!ec_role_allows(EC_ROLE_OPERATOR, EC_ROLE_ADMIN));
  EXPECT_TRUE(!ec_role_allows(EC_ROLE_ANY, EC_ROLE_ADMIN));
}

// ── command tree ────────────────────────────────────────────

static ec_command_spec mk(const char *path, const char *help,
                          ec_role_gate role) {
  ec_command_spec s;
  memset(&s, 0, sizeof(s));
  snprintf(s.path, sizeof(s.path), "%s", path);
  snprintf(s.help, sizeof(s.help), "%s", help);
  snprintf(s.wire_command, sizeof(s.wire_command), "%s", path);
  s.role = role;
  return s;
}

static ec_command_tree sample_tree(void) {
  ec_command_tree t;
  ec_ct_init(&t);
  ec_command_spec s;
  s = mk("show interfaces", "Show ports", EC_ROLE_ANY);
  s.args[0] = (ec_arg_spec){.name = "port", .help = "Port",
                            .required = false};
  s.arg_count = 1;
  ec_ct_register(&t, &s);
  s = mk("show version", "Show version", EC_ROLE_ANY);
  ec_ct_register(&t, &s);
  s = mk("set", "Set config value", EC_ROLE_ADMIN);
  ec_ct_register(&t, &s);
  s = mk("commit", "Apply config", EC_ROLE_OPERATOR);
  ec_ct_register(&t, &s);
  s = mk("compare", "Compare configs", EC_ROLE_OPERATOR);
  ec_ct_register(&t, &s);
  return t;
}

static void test_parse_exact(void) {
  ec_command_tree t = sample_tree();
  const char *tok[] = {"show", "interfaces"};
  ec_parsed_command pc;
  ec_error err;
  EXPECT_EQ(ec_ct_parse(&t, tok, 2, EC_ROLE_ANY, &pc, &err),
            EC_OK);
  EXPECT_STREQ(pc.spec->path, "show interfaces");
}

static void test_parse_args(void) {
  ec_command_tree t = sample_tree();
  const char *tok[] = {"show", "interfaces", "ge1"};
  ec_parsed_command pc;
  ec_error err;
  EXPECT_EQ(ec_ct_parse(&t, tok, 3, EC_ROLE_ANY, &pc, &err),
            EC_OK);
  EXPECT_EQ(pc.arg_count, 1u);
  EXPECT_STREQ(pc.args[0], "ge1");
}

static void test_parse_prefix(void) {
  ec_command_tree t = sample_tree();
  const char *tok[] = {"sh", "int"};
  ec_parsed_command pc;
  ec_error err;
  EXPECT_EQ(ec_ct_parse(&t, tok, 2, EC_ROLE_ANY, &pc, &err),
            EC_OK);
  EXPECT_STREQ(pc.spec->path, "show interfaces");
}

static void test_parse_ambiguous(void) {
  ec_command_tree t = sample_tree();
  const char *tok[] = {"com"};
  ec_parsed_command pc;
  ec_error err;
  EXPECT_EQ(ec_ct_parse(&t, tok, 1, EC_ROLE_ADMIN, &pc, &err),
            (ec_error_code)EC_ERR_UNKNOWN_COMMAND);
  EXPECT_TRUE(strstr(err.message, "ambiguous") != NULL);
}

static void test_parse_role_gate(void) {
  ec_command_tree t = sample_tree();
  const char *tok[] = {"set"};
  ec_parsed_command pc;
  ec_error err;
  EXPECT_EQ(ec_ct_parse(&t, tok, 1, EC_ROLE_OPERATOR,
                         &pc, &err),
            (ec_error_code)EC_ERR_NOT_AUTHORISED);
}

static void test_parse_unknown_suggests(void) {
  ec_command_tree t = sample_tree();
  const char *tok[] = {"shw"};
  ec_parsed_command pc;
  ec_error err;
  ec_ct_parse(&t, tok, 1, EC_ROLE_ANY, &pc, &err);
  EXPECT_TRUE(strstr(err.message, "show") != NULL);
}

static void test_completions(void) {
  ec_command_tree t = sample_tree();
  char out[8][EC_MAX_TOKEN];
  const char *prec[] = {"show"};
  size_t n = ec_ct_suggest(&t, prec, 1, "", out, 8);
  EXPECT_EQ(n, 2u);
  EXPECT_STREQ(out[0], "interfaces");
  EXPECT_STREQ(out[1], "version");
}

// ── table ───────────────────────────────────────────────────

static void test_table(void) {
  ec_table t;
  ec_table_init(&t);
  ec_table_add_column(&t, "port", EC_ALIGN_LEFT);
  ec_table_add_column(&t, "status", EC_ALIGN_LEFT);
  ec_table_add_column(&t, "speed", EC_ALIGN_RIGHT);

  ec_cell r1[] = {
      ec_cell_make("ge1", EC_SEM_EMPHASIS),
      ec_cell_make("up", EC_SEM_GOOD),
      ec_cell_make("100M", EC_SEM_DEFAULT),
  };
  ec_table_add_row(&t, r1, 3);

  ec_cell r2[] = {
      ec_cell_make("ge2", EC_SEM_EMPHASIS),
      ec_cell_make("down", EC_SEM_BAD),
      ec_cell_make("-", EC_SEM_DIM),
  };
  ec_table_add_row(&t, r2, 3);

  char s[1024];
  ec_buf b = {s, sizeof(s), 0};
  ec_table_render(&t, &b);
  EXPECT_TRUE(b.used > 0);
  EXPECT_TRUE(strstr(s, "port") != NULL);
  EXPECT_TRUE(strstr(s, "[OK] up") != NULL);
  EXPECT_TRUE(strstr(s, "[FAIL] down") != NULL);
  EXPECT_TRUE(strstr(s, "[--] -") != NULL);
  EXPECT_TRUE(strstr(s, "---") != NULL);
}

static void test_table_alignment(void) {
  ec_table t;
  ec_table_init(&t);
  ec_table_add_column(&t, "name", EC_ALIGN_LEFT);
  ec_table_add_column(&t, "count", EC_ALIGN_RIGHT);
  ec_cell r[] = {
      ec_cell_make("rx", EC_SEM_DEFAULT),
      ec_cell_make("42", EC_SEM_DEFAULT),
  };
  ec_table_add_row(&t, r, 2);
  char s[512];
  ec_buf b = {s, sizeof(s), 0};
  ec_table_render(&t, &b);
  EXPECT_TRUE(strstr(s, "42") != NULL);
}

// ── help ────────────────────────────────────────────────────

static void test_help_index(void) {
  ec_command_tree t = sample_tree();
  char s[2048];
  ec_buf b = {s, sizeof(s), 0};
  ec_ct_format_help(&t, &b);
  EXPECT_TRUE(strstr(s, "show interfaces") != NULL);
  EXPECT_TRUE(strstr(s, "commit") != NULL);
}

// ── history ─────────────────────────────────────────────────

static void test_history(void) {
  ec_history h;
  ec_history_init(&h);
  ec_history_add(&h, "show interfaces");
  ec_history_add(&h, "show version");
  EXPECT_EQ(h.count, 2u);
  EXPECT_STREQ(h.lines[0], "show interfaces");
  EXPECT_STREQ(h.lines[1], "show version");
}

static void test_history_wrap(void) {
  ec_history h;
  ec_history_init(&h);
  for (int i = 0; i < EC_HISTORY_SIZE + 4; ++i) {
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "cmd%d", i);
    ec_history_add(&h, cmd);
  }
  EXPECT_EQ(h.count, (size_t)EC_HISTORY_SIZE);
}

int main(void) {
  RUN(buf_basic);
  RUN(buf_truncate);
  RUN(distance);
  RUN(suggest);
  RUN(roles);
  RUN(parse_exact);
  RUN(parse_args);
  RUN(parse_prefix);
  RUN(parse_ambiguous);
  RUN(parse_role_gate);
  RUN(parse_unknown_suggests);
  RUN(completions);
  RUN(table);
  RUN(table_alignment);
  RUN(help_index);
  RUN(history);
  RUN(history_wrap);
  printf("\n%d tests, %d failures\n", tests, failures);
  return failures ? 1 : 0;
}
