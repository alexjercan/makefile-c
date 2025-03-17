#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/wait.h>
#include "ds.h"

typedef enum make_token_kind {
    MAKE_TOKEN_TARGET,
    MAKE_TOKEN_COLON,
    MAKE_TOKEN_SEMICOLON,
    MAKE_TOKEN_EQUALS,
    MAKE_TOKEN_CMD,
    MAKE_TOKEN_EOF,
    MAKE_TOKEN_ILLEGAL,
} make_token_kind;

static const char* make_token_kind_to_string(make_token_kind kind) {
    switch (kind) {
    case MAKE_TOKEN_TARGET: return "TARGET";
    case MAKE_TOKEN_COLON: return ":";
    case MAKE_TOKEN_SEMICOLON: return ";";
    case MAKE_TOKEN_EQUALS: return "=";
    case MAKE_TOKEN_CMD: return "CMD";
    case MAKE_TOKEN_EOF: return "<EOF>";
    case MAKE_TOKEN_ILLEGAL: return "ILLEGAL";
    }
}

typedef struct make_token {
    make_token_kind kind;
    ds_string_slice value;
    unsigned int pos;
} make_token;

static void make_token_printf(make_token token) {
    printf("%s", make_token_kind_to_string(token.kind));
    if (token.value.len > 0) {
        printf("(%.*s)", token.value.len, token.value.str);
    }
    printf("\n");
}

typedef struct make_lexer {
    ds_string_slice buffer;
    unsigned int pos;
    unsigned int read_pos;
    char ch;
} make_lexer;

static char make_lexer_peek_ch(make_lexer *lexer) {
    if (lexer->read_pos >= lexer->buffer.len) {
        return EOF;
    }

    return lexer->buffer.str[lexer->read_pos];
}

static char make_lexer_read(make_lexer *lexer) {
    lexer->ch = make_lexer_peek_ch(lexer);

    lexer->pos = lexer->read_pos;
    lexer->read_pos += 1;

    return lexer->ch;
}

static void make_lexer_skip_whitespace(make_lexer *lexer) {
    while (isspace(lexer->ch)) {
        make_lexer_read(lexer);
    }
}

static int make_lexer_init(make_lexer *lexer, ds_string_slice buffer) {
    lexer->buffer = buffer;
    lexer->pos = 0;
    lexer->read_pos = 0;
    lexer->ch = 0;

    make_lexer_read(lexer);

    return 0;
}

static bool istargetch(char ch) {
    return isalnum(ch) || ch == '.' || ch == '_';
}

static int make_lexer_tokenize_target(make_lexer *lexer, make_token *token) {
    int result = 0;
    unsigned int position = lexer->pos;

    if (!istargetch(lexer->ch)) {
        DS_LOG_ERROR("Failed to parse ident: expected [a-zA-Z0-9._] but got '%c'", lexer->ch);
        return_defer(1);
    }

    ds_string_slice slice = { .str = (char *)lexer->buffer.str + lexer->pos, .len = 0 };
        while (istargetch(lexer->ch)) {
        slice.len += 1;
        make_lexer_read(lexer);
    }

    *token = (make_token){.kind = MAKE_TOKEN_TARGET, .value = slice, .pos = position };

defer:
    return result;
}

static int make_lexer_tokenize_string(make_lexer *lexer, make_token *token) {
    int result = 0;
    unsigned int position = lexer->pos;

    if (lexer->ch != '"') {
        DS_LOG_ERROR("Failed to parse string: expected '\"' but got '%c'", lexer->ch);
        return_defer(1);
    }

    make_lexer_read(lexer);

    ds_string_slice slice = { .str = (char *)lexer->buffer.str + lexer->pos, .len = 0 };
    while (lexer->ch != '"') {
        char ch = lexer->ch;
        slice.len += 1;
        make_lexer_read(lexer);

        if (ch == '\\' && lexer->ch == '"') {
            slice.len += 1;
            make_lexer_read(lexer);
        }
    }

    make_lexer_read(lexer);

    *token = (make_token){.kind = MAKE_TOKEN_CMD, .value = slice, .pos = position };

defer:
    return result;
}

static int make_lexer_next(make_lexer *lexer, make_token *token) {
    int result = 0;
    make_lexer_skip_whitespace(lexer);

    unsigned int position = lexer->pos;
    if (lexer->ch == EOF) {
        make_lexer_read(lexer);
        *token = (make_token){.kind = MAKE_TOKEN_EOF, .value = {0}, .pos = position };
        return_defer(0);
    } else if (lexer->ch == ':') {
        make_lexer_read(lexer);
        *token = (make_token){.kind = MAKE_TOKEN_COLON, .value = {0}, .pos = position };
        return_defer(0);
    } else if (lexer->ch == ';') {
        make_lexer_read(lexer);
        *token = (make_token){.kind = MAKE_TOKEN_SEMICOLON, .value = {0}, .pos = position };
        return_defer(0);
    } else if (lexer->ch == '=') {
        make_lexer_read(lexer);
        *token = (make_token){.kind = MAKE_TOKEN_EQUALS, .value = {0}, .pos = position };
        return_defer(0);
    } else if (lexer->ch == '"') {
        return_defer(make_lexer_tokenize_string(lexer, token));
    } else if (istargetch(lexer->ch)) {
        return_defer(make_lexer_tokenize_target(lexer, token));
    } else {
        ds_string_slice slice = { .str = (char *)lexer->buffer.str + lexer->pos, .len = 1 };

        make_lexer_read(lexer);

        *token = (make_token){.kind = MAKE_TOKEN_ILLEGAL, .value = slice, .pos = position };

        return_defer(0);
    }

defer:
    return result;
}

static int make_lexer_pos_to_lc(make_lexer *lexer, unsigned int pos, int *line, int *column) {
    int result = 0;
    int n = (pos > lexer->buffer.len) ? lexer->buffer.len : pos;

    *line = 1;
    *column = 1;

    for (int i = 0; i < n; i++) {
        if (lexer->buffer.str[i] == '\n') {
            *line += 1;
            *column = 0;
        } else {
            *column += 1;
        }
    }

    return result;
}

static void make_lexer_free(make_lexer *lexer) {
    lexer->pos = 0;
    lexer->read_pos = 0;
    lexer->ch = 0;
}

typedef struct make_parser {
    make_lexer lexer;
    make_token tok;
    make_token next_tok;
} make_parser;

static make_token make_parser_read(make_parser *parser) {
    parser->tok = parser->next_tok;
    make_lexer_next(&parser->lexer, &parser->next_tok);

    return parser->tok;
}

static int make_parser_init(make_parser *parser, make_lexer lexer) {
    parser->lexer = lexer;

    make_parser_read(parser);
    make_parser_read(parser);

    return 0;
}

typedef struct make_rule {
    ds_string_slice target;
    ds_dynamic_array deps; /* ds_string_slice */
    ds_string_slice cmd;
} make_rule;

static void make_rule_printf(make_rule rule) {
    printf("%.*s:", rule.target.len, rule.target.str);
    for (unsigned int i = 0; i < rule.deps.count; i++) {
        ds_string_slice dep = {0};
        DS_UNREACHABLE(ds_dynamic_array_get(&rule.deps, i, &dep));
        printf(" %.*s", dep.len, dep.str);
    }

    if (rule.cmd.len > 0) {
        printf(" = \"%.*s\"", rule.cmd.len, rule.cmd.str);
    }

    printf("\n");
}

static int make_parser_parse_rule(make_parser *parser, make_rule *rule) {
    make_token token = {0};
    int result = 0;
    ds_dynamic_array_init(&rule->deps, sizeof(ds_string_slice));

    token = parser->tok;
    if (token.kind != MAKE_TOKEN_TARGET) {
        int line, column;
        make_lexer_pos_to_lc(&parser->lexer, token.pos, &line, &column);
        DS_LOG_ERROR("Expected a target but found %s at %d:%d", make_token_kind_to_string(token.kind), line, column);
        return_defer(1);
    }
    rule->target = token.value;

    token = make_parser_read(parser);
    if (token.kind != MAKE_TOKEN_COLON) {
        int line, column;
        make_lexer_pos_to_lc(&parser->lexer, token.pos, &line, &column);
        DS_LOG_ERROR("Expected a `:` but found %s at %d:%d", make_token_kind_to_string(token.kind), line, column);
        return_defer(1);
    }

    while (true) {
        token = make_parser_read(parser);
        if (token.kind == MAKE_TOKEN_SEMICOLON || token.kind == MAKE_TOKEN_EQUALS) {
            break;
        }

        if (token.kind != MAKE_TOKEN_TARGET) {
            int line, column;
            make_lexer_pos_to_lc(&parser->lexer, token.pos, &line, &column);
            DS_LOG_ERROR("Expected a target but found %s at %d:%d", make_token_kind_to_string(token.kind), line, column);
            return_defer(1);
        }
        DS_UNREACHABLE(ds_dynamic_array_append(&rule->deps, &token.value));
    }

    if (token.kind == MAKE_TOKEN_SEMICOLON) {
        token = make_parser_read(parser);
        return_defer(0);
    }

    token = make_parser_read(parser);
    if (token.kind != MAKE_TOKEN_CMD) {
        int line, column;
        make_lexer_pos_to_lc(&parser->lexer, token.pos, &line, &column);
        DS_LOG_ERROR("Expected a cmd but found %s at %d:%d", make_token_kind_to_string(token.kind), line, column);
        return_defer(1);
    }
    rule->cmd = token.value;

    token = make_parser_read(parser);
    if (token.kind != MAKE_TOKEN_SEMICOLON) {
        int line, column;
        make_lexer_pos_to_lc(&parser->lexer, token.pos, &line, &column);
        DS_LOG_ERROR("Expected a `;` but found %s at %d:%d", make_token_kind_to_string(token.kind), line, column);
        return_defer(1);
    }

    make_parser_read(parser);

defer:
    return result;
}

typedef struct make_file {
    ds_dynamic_array rules; /* make_rule */
} make_file;

static void make_file_printf(make_file make) {
    for (unsigned int i = 0; i < make.rules.count; i++) {
        make_rule rule = {0};
        DS_UNREACHABLE(ds_dynamic_array_get(&make.rules, i, &rule));

        make_rule_printf(rule);
    }
}

static int make_parser_parse_file(make_parser *parser, make_file *make) {
    int result = 0;
    ds_dynamic_array_init(&make->rules, sizeof(make_rule));

    while (true) {
        if (parser->tok.kind == MAKE_TOKEN_EOF) {
            break;
        }

        make_rule rule = {0};
        if (make_parser_parse_rule(parser, &rule) != 0) {
            return_defer(1);
        }
        DS_UNREACHABLE(ds_dynamic_array_append(&make->rules, &rule));
    }

defer:
    return result;
}

static void execute_command(ds_string_slice command) {
    pid_t pid;
    int status = 0;
    pid = fork();

    if (pid == 0) {
        char *cmd = NULL;

        ds_dynamic_array args = {0};
        ds_dynamic_array_init(&args, sizeof(char *));

        ds_string_slice token = {0};
        while (ds_string_slice_tokenize(&command, ' ', &token) == 0) {
            char *buffer = NULL;
            DS_UNREACHABLE(ds_string_slice_to_owned(&token, &buffer));

            if (cmd == NULL) cmd = buffer;
            DS_UNREACHABLE(ds_dynamic_array_append(&args, &buffer));
        }

        char **argv = args.items;
        argv[args.count] = NULL;

        if (execvp(cmd, argv) == -1) {
            DS_PANIC("execvp");
        }
    } else if (pid > 0) {
        if (waitpid(pid, &status, 0) == -1) {
            DS_PANIC("waitpid");
        }
    } else {
        DS_PANIC("fork");
    }
}

static void make_plan_dfs(make_file make, make_rule rule) {
    struct stat statbuf;

    char *pathname = NULL;
    DS_UNREACHABLE(ds_string_slice_to_owned(&rule.target, &pathname));

    int target_time = 0;
    if (stat(pathname, &statbuf) == 0) {
        target_time = statbuf.st_mtim.tv_sec;
    }

    bool target_newer = true;
    for (unsigned int i = 0; i < rule.deps.count; i++) {
        ds_string_slice dep = {0};
        DS_UNREACHABLE(ds_dynamic_array_get(&rule.deps, i, &dep));

        make_rule dep_rule = {0};
        bool found = false;
        for (unsigned int j = 0; j < make.rules.count; j++) {
            DS_UNREACHABLE(ds_dynamic_array_get(&make.rules, j, &dep_rule));
            if (ds_string_slice_equals(&dep, &dep_rule.target)) {
                found = true;
                break;
            }
        }

        if (found) {
            make_plan_dfs(make, dep_rule);
        }

        char *pathname = NULL;
        DS_UNREACHABLE(ds_string_slice_to_owned(&dep, &pathname));

        int dep_time = 0;
        if (stat(pathname, &statbuf) == 0) {
            dep_time = statbuf.st_mtim.tv_sec;
        }

        if (dep_time > target_time) {
            target_newer = false;
        }
    }

    if (!target_newer) {
        DS_LOG_INFO("run %.*s: %.*s", rule.target.len, rule.target.str, rule.cmd.len, rule.cmd.str);
        execute_command(rule.cmd);
    }
}

static void make_plan(make_file make) {
    make_rule rule = {0};
    DS_UNREACHABLE(ds_dynamic_array_get(&make.rules, 0, &rule));

    make_plan_dfs(make, rule);
}

typedef struct arguments {
    char *file;
} arguments;

void parse_arguments(int argc, char **argv, arguments *args) {
    ds_argparse_parser parser = {0};
    ds_argparse_parser_init(
        &parser,
        "make",
        "a clone of the make cli tool",
        "0.1"
    );

    ds_argparse_add_argument(&parser, (ds_argparse_options){
        .short_name = 'f',
        .long_name = "file",
        .description = "the file to use for make",
        .type = ARGUMENT_TYPE_POSITIONAL,
        .required = true,
    });

    DS_UNREACHABLE(ds_argparse_parse(&parser, argc, argv));

    args->file = ds_argparse_get_value(&parser, "file");

    ds_argparse_parser_free(&parser);
}

int main(int argc, char **argv) {
    arguments args = {0};
    parse_arguments(argc, argv, &args);

    char *content = NULL;
    int size = ds_io_read(args.file, &content, "r");

    ds_string_slice slice = {0};
    ds_string_slice_init(&slice, content, size);

    make_lexer lexer = {0};
    make_lexer_init(&lexer, slice);

    make_parser parser = {0};
    make_parser_init(&parser, lexer);

    make_file make = {0};
    if (make_parser_parse_file(&parser, &make) != 0) {
        return 1;
    }

    make_plan(make);

    make_lexer_free(&lexer);

    return 0;
}
