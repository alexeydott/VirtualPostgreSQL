#include "vps_query_source.h"

#include <ctype.h>
#include <string.h>

#define VPS_QUERY_HASH_OFFSET UINT64_C(1469598103934665603)
#define VPS_QUERY_HASH_PRIME UINT64_C(1099511628211)

typedef struct VpsQueryWord {
    size_t offset;
    size_t length;
} VpsQueryWord;

static uint64_t vps_query_hash_byte(uint64_t hash, unsigned char value)
{
    return (hash ^ (uint64_t)value) * VPS_QUERY_HASH_PRIME;
}

static int vps_query_word_equal(const char *query,
                                const VpsQueryWord *word,
                                const char *expected)
{
    size_t index;
    size_t expected_length = strlen(expected);
    if (word->length != expected_length) return 0;
    for (index = 0U; index < word->length; ++index) {
        unsigned char value = (unsigned char)query[word->offset + index];
        if ((unsigned char)tolower(value) != (unsigned char)expected[index]) {
            return 0;
        }
    }
    return 1;
}

static int vps_query_is_identifier_start(unsigned char value)
{
    return value == '_' || value >= 0x80U || isalpha(value) != 0;
}

static int vps_query_is_identifier_continue(unsigned char value)
{
    return value == '_' || value == '$' || value >= 0x80U ||
           isalnum(value) != 0;
}

static int vps_query_utf8_sequence_length(const unsigned char *bytes,
                                          size_t remaining,
                                          size_t *sequence_length)
{
    unsigned char first;
    uint32_t codepoint;
    size_t length;
    size_t index;
    if (remaining == 0U || sequence_length == NULL) return 0;
    first = bytes[0];
    if (first < 0x80U) {
        *sequence_length = 1U;
        return 1;
    }
    if (first >= 0xc2U && first <= 0xdfU) {
        length = 2U;
        codepoint = (uint32_t)(first & 0x1fU);
    } else if (first >= 0xe0U && first <= 0xefU) {
        length = 3U;
        codepoint = (uint32_t)(first & 0x0fU);
    } else if (first >= 0xf0U && first <= 0xf4U) {
        length = 4U;
        codepoint = (uint32_t)(first & 0x07U);
    } else {
        return 0;
    }
    if (remaining < length) return 0;
    for (index = 1U; index < length; ++index) {
        if ((bytes[index] & 0xc0U) != 0x80U) return 0;
        codepoint = (codepoint << 6) | (uint32_t)(bytes[index] & 0x3fU);
    }
    if ((length == 3U && codepoint < UINT32_C(0x800)) ||
        (length == 4U && codepoint < UINT32_C(0x10000)) ||
        codepoint > UINT32_C(0x10ffff) ||
        (codepoint >= UINT32_C(0xd800) && codepoint <= UINT32_C(0xdfff))) {
        return 0;
    }
    *sequence_length = length;
    return 1;
}

static int vps_query_dollar_delimiter(const char *query,
                                      size_t query_length,
                                      size_t offset,
                                      size_t *delimiter_length)
{
    size_t index;
    if (offset >= query_length || query[offset] != '$') return 0;
    index = offset + 1U;
    while (index < query_length &&
           (query[index] == '_' || isalnum((unsigned char)query[index]) != 0)) {
        ++index;
    }
    if (index >= query_length || query[index] != '$') return 0;
    *delimiter_length = index - offset + 1U;
    return 1;
}

static int vps_query_forbidden_word(const char *query,
                                    const VpsQueryWord *word)
{
    static const char *const words[] = {
        "insert", "update", "delete", "merge", "copy", "call", "do",
        "alter", "create", "drop", "truncate", "grant", "revoke",
        "vacuum", "cluster", "reindex", "refresh", "lock", "listen",
        "unlisten", "notify", "set", "reset", "discard", "prepare",
        "execute", "deallocate", "begin", "start", "commit", "rollback",
        "savepoint", "release"
    };
    size_t index;
    for (index = 0U; index < sizeof(words) / sizeof(words[0]); ++index) {
        if (vps_query_word_equal(query, word, words[index])) return 1;
    }
    return 0;
}

static void vps_query_log(VpsLogger *logger,
                          const VpsQuerySourceAnalysis *analysis)
{
    VpsLogEvent event;
    const char *result_name;
    if (logger == NULL || analysis == NULL) return;
    result_name = vps_query_source_result_name(analysis->result);
    if (vps_log_event_init(&event,
            analysis->result == VPS_QUERY_SOURCE_OK ? VPS_LOG_LEVEL_DEBUG
                                                    : VPS_LOG_LEVEL_WARN) != VPS_LOG_OK) return;
    (void)vps_log_event_add_string(&event, VPS_LOG_FIELD_OPERATION,
                                   "query_scan", sizeof("query_scan") - 1U);
    (void)vps_log_event_add_string(&event, VPS_LOG_FIELD_PHASE,
                                   vps_query_source_root_name(analysis->root),
                                   strlen(vps_query_source_root_name(analysis->root)));
    (void)vps_log_event_add_string(&event, VPS_LOG_FIELD_STATUS,
                                   result_name, strlen(result_name));
    (void)vps_log_event_add_uint64(&event, VPS_LOG_FIELD_BYTE_COUNT,
                                   (uint64_t)analysis->error_offset);
    (void)vps_log_event_add_uint64(&event, VPS_LOG_FIELD_ROW_COUNT,
                                   (uint64_t)analysis->token_count);
    (void)vps_log_event_add_uint64(&event, VPS_LOG_FIELD_QUERY_FINGERPRINT,
                                   analysis->normalized_hash);
    vps_logger_emit(logger, &event);
}

static VpsQuerySourceResult vps_query_fail(VpsQuerySourceAnalysis *analysis,
                                           VpsQuerySourceResult result,
                                           size_t offset,
                                           VpsLogger *logger)
{
    analysis->result = result;
    analysis->error_offset = offset;
    vps_query_log(logger, analysis);
    return result;
}

VpsQuerySourceResult vps_query_source_scan(
    const char *query,
    size_t query_length,
    VpsLogger *logger,
    VpsQuerySourceAnalysis *analysis)
{
    size_t offset = 0U;
    size_t depth = 0U;
    size_t sequence_length;
    VpsQueryWord previous = {0U, 0U};
    VpsQueryWord previous_previous = {0U, 0U};
    int saw_word = 0;
    int saw_select = 0;
    int terminal = 0;

    if (analysis == NULL) return VPS_QUERY_SOURCE_INVALID_ARGUMENT;
    (void)memset(analysis, 0, sizeof(*analysis));
    analysis->format_version = VPS_QUERY_SOURCE_FORMAT_VERSION;
    analysis->result = VPS_QUERY_SOURCE_INVALID_ARGUMENT;
    analysis->normalized_hash = VPS_QUERY_HASH_OFFSET;
    analysis->statement_length = query_length;
    if (query == NULL || query_length == 0U) {
        return vps_query_fail(analysis, VPS_QUERY_SOURCE_INVALID_ARGUMENT, 0U,
                              logger);
    }
    if (query_length > VPS_QUERY_SOURCE_MAX_BYTES) {
        return vps_query_fail(analysis, VPS_QUERY_SOURCE_LIMIT_EXCEEDED,
                              query_length, logger);
    }

    while (offset < query_length) {
        unsigned char value = (unsigned char)query[offset];
        if (value == 0U) {
            return vps_query_fail(analysis, VPS_QUERY_SOURCE_CONTAINS_NUL,
                                  offset, logger);
        }
        if (!vps_query_utf8_sequence_length(
                (const unsigned char *)query + offset,
                query_length - offset, &sequence_length)) {
            return vps_query_fail(analysis, VPS_QUERY_SOURCE_INVALID_UTF8,
                                  offset, logger);
        }
        if (value >= 0x80U) {
            size_t sequence_offset;
            for (sequence_offset = 0U; sequence_offset < sequence_length;
                 ++sequence_offset) {
                analysis->normalized_hash = vps_query_hash_byte(
                    analysis->normalized_hash,
                    (unsigned char)query[offset + sequence_offset]);
            }
            offset += sequence_length;
            continue;
        }
        if (isspace(value) != 0) {
            ++offset;
            continue;
        }
        if (query[offset] == '-' && offset + 1U < query_length &&
            query[offset + 1U] == '-') {
            offset += 2U;
            while (offset < query_length && query[offset] != '\n' &&
                   query[offset] != '\r') ++offset;
            continue;
        }
        if (query[offset] == '/' && offset + 1U < query_length &&
            query[offset + 1U] == '*') {
            size_t comment_depth = 1U;
            offset += 2U;
            while (offset < query_length && comment_depth != 0U) {
                if (query[offset] == '/' && offset + 1U < query_length &&
                    query[offset + 1U] == '*') {
                    if (++comment_depth > VPS_QUERY_SOURCE_MAX_NESTING) {
                        return vps_query_fail(analysis,
                            VPS_QUERY_SOURCE_LIMIT_EXCEEDED, offset, logger);
                    }
                    offset += 2U;
                } else if (query[offset] == '*' &&
                           offset + 1U < query_length &&
                           query[offset + 1U] == '/') {
                    --comment_depth;
                    offset += 2U;
                } else {
                    ++offset;
                }
            }
            if (comment_depth != 0U) {
                return vps_query_fail(analysis, VPS_QUERY_SOURCE_UNTERMINATED,
                                      offset, logger);
            }
            continue;
        }
        if (terminal) {
            return vps_query_fail(analysis,
                                  VPS_QUERY_SOURCE_MULTIPLE_STATEMENTS,
                                  offset, logger);
        }
        if (query[offset] == ';') {
            analysis->statement_length = offset;
            terminal = 1;
            analysis->has_terminal_semicolon = 1;
            ++offset;
            continue;
        }
        if (query[offset] == '\'' || query[offset] == '"') {
            char quote = query[offset];
            analysis->normalized_hash = vps_query_hash_byte(
                analysis->normalized_hash, (unsigned char)quote);
            ++offset;
            while (offset < query_length) {
                if (query[offset] == quote) {
                    if (offset + 1U < query_length &&
                        query[offset + 1U] == quote) {
                        analysis->normalized_hash = vps_query_hash_byte(
                            analysis->normalized_hash, (unsigned char)quote);
                        offset += 2U;
                        continue;
                    }
                    ++offset;
                    break;
                }
                if (quote == '\'' && query[offset] == '\\' &&
                    offset + 1U < query_length) {
                    analysis->normalized_hash = vps_query_hash_byte(
                        analysis->normalized_hash,
                        (unsigned char)query[offset + 1U]);
                    offset += 2U;
                } else {
                    analysis->normalized_hash = vps_query_hash_byte(
                        analysis->normalized_hash,
                        (unsigned char)query[offset]);
                    ++offset;
                }
            }
            if (offset > query_length || query[offset - 1U] != quote) {
                return vps_query_fail(analysis, VPS_QUERY_SOURCE_UNTERMINATED,
                                      query_length, logger);
            }
            ++analysis->token_count;
            continue;
        }
        if (query[offset] == '$') {
            size_t delimiter_length = 0U;
            if (offset + 1U < query_length &&
                isdigit((unsigned char)query[offset + 1U]) != 0) {
                return vps_query_fail(analysis,
                    VPS_QUERY_SOURCE_UNRESOLVED_PARAMETER, offset, logger);
            }
            if (vps_query_dollar_delimiter(query, query_length, offset,
                                           &delimiter_length)) {
                size_t content = offset + delimiter_length;
                size_t end = content;
                while (end + delimiter_length <= query_length &&
                       memcmp(query + end, query + offset,
                              delimiter_length) != 0) ++end;
                if (end + delimiter_length > query_length) {
                    return vps_query_fail(analysis,
                        VPS_QUERY_SOURCE_UNTERMINATED, offset, logger);
                }
                analysis->normalized_hash = vps_query_hash_byte(
                    analysis->normalized_hash, (unsigned char)'$');
                while (content < end) {
                    analysis->normalized_hash = vps_query_hash_byte(
                        analysis->normalized_hash,
                        (unsigned char)query[content++]);
                }
                offset = end + delimiter_length;
                ++analysis->token_count;
                continue;
            }
        }
        if (query[offset] == '(') {
            if (++depth > VPS_QUERY_SOURCE_MAX_NESTING) {
                return vps_query_fail(analysis, VPS_QUERY_SOURCE_LIMIT_EXCEEDED,
                                      offset, logger);
            }
            if (depth > analysis->maximum_nesting) {
                analysis->maximum_nesting = depth;
            }
        } else if (query[offset] == ')') {
            if (depth == 0U) {
                return vps_query_fail(analysis, VPS_QUERY_SOURCE_UNBALANCED,
                                      offset, logger);
            }
            --depth;
        }
        if (vps_query_is_identifier_start(value)) {
            VpsQueryWord word;
            size_t index;
            word.offset = offset;
            ++offset;
            while (offset < query_length && vps_query_is_identifier_continue(
                       (unsigned char)query[offset])) ++offset;
            word.length = offset - word.offset;
            if (++analysis->token_count > VPS_QUERY_SOURCE_MAX_TOKENS) {
                return vps_query_fail(analysis, VPS_QUERY_SOURCE_LIMIT_EXCEEDED,
                                      word.offset, logger);
            }
            for (index = 0U; index < word.length; ++index) {
                unsigned char byte = (unsigned char)query[word.offset + index];
                analysis->normalized_hash = vps_query_hash_byte(
                    analysis->normalized_hash,
                    byte < 0x80U ? (unsigned char)tolower(byte) : byte);
            }
            analysis->normalized_hash = vps_query_hash_byte(
                analysis->normalized_hash, 0U);
            if (!saw_word) {
                saw_word = 1;
                if (vps_query_word_equal(query, &word, "select")) {
                    analysis->root = VPS_QUERY_ROOT_SELECT;
                    saw_select = 1;
                } else if (vps_query_word_equal(query, &word, "with")) {
                    analysis->root = VPS_QUERY_ROOT_WITH_SELECT;
                } else {
                    return vps_query_fail(analysis,
                        VPS_QUERY_SOURCE_UNSUPPORTED_ROOT, word.offset, logger);
                }
            } else if (vps_query_word_equal(query, &word, "select")) {
                saw_select = 1;
            }
            if ((vps_query_word_equal(query, &previous, "for") &&
                 (vps_query_word_equal(query, &word, "update") ||
                  vps_query_word_equal(query, &word, "share"))) ||
                (vps_query_word_equal(query, &previous_previous, "for") &&
                 vps_query_word_equal(query, &previous, "key") &&
                 (vps_query_word_equal(query, &word, "update") ||
                  vps_query_word_equal(query, &word, "share")))) {
                return vps_query_fail(analysis, VPS_QUERY_SOURCE_LOCKING_SELECT,
                                      previous.offset, logger);
            }
            if (vps_query_forbidden_word(query, &word)) {
                VpsQuerySourceResult result =
                    analysis->root == VPS_QUERY_ROOT_WITH_SELECT && !saw_select
                        ? VPS_QUERY_SOURCE_DATA_MODIFYING_CTE
                        : VPS_QUERY_SOURCE_FORBIDDEN_COMMAND;
                return vps_query_fail(analysis, result, word.offset, logger);
            }
            if (saw_select && vps_query_word_equal(query, &word, "into")) {
                return vps_query_fail(analysis, VPS_QUERY_SOURCE_SELECT_INTO,
                                      word.offset, logger);
            }
            if (vps_query_word_equal(query, &previous_previous, "for") &&
                vps_query_word_equal(query, &previous, "no") &&
                vps_query_word_equal(query, &word, "key")) {
                return vps_query_fail(analysis, VPS_QUERY_SOURCE_LOCKING_SELECT,
                                      previous_previous.offset, logger);
            }
            previous_previous = previous;
            previous = word;
            continue;
        }
        analysis->normalized_hash = vps_query_hash_byte(
            analysis->normalized_hash, value);
        if (++analysis->token_count > VPS_QUERY_SOURCE_MAX_TOKENS) {
            return vps_query_fail(analysis, VPS_QUERY_SOURCE_LIMIT_EXCEEDED,
                                  offset, logger);
        }
        ++offset;
    }
    if (!saw_word || !saw_select) {
        return vps_query_fail(analysis, VPS_QUERY_SOURCE_UNSUPPORTED_ROOT,
                              query_length, logger);
    }
    if (depth != 0U) {
        return vps_query_fail(analysis, VPS_QUERY_SOURCE_UNBALANCED,
                              query_length, logger);
    }
    analysis->result = VPS_QUERY_SOURCE_OK;
    analysis->error_offset = query_length;
    vps_query_log(logger, analysis);
    return VPS_QUERY_SOURCE_OK;
}

const char *vps_query_source_result_name(VpsQuerySourceResult result)
{
    switch (result) {
    case VPS_QUERY_SOURCE_OK: return "ok";
    case VPS_QUERY_SOURCE_INVALID_ARGUMENT: return "invalid_argument";
    case VPS_QUERY_SOURCE_LIMIT_EXCEEDED: return "limit_exceeded";
    case VPS_QUERY_SOURCE_INVALID_UTF8: return "invalid_utf8";
    case VPS_QUERY_SOURCE_CONTAINS_NUL: return "contains_nul";
    case VPS_QUERY_SOURCE_UNTERMINATED: return "unterminated";
    case VPS_QUERY_SOURCE_MULTIPLE_STATEMENTS: return "multiple_statements";
    case VPS_QUERY_SOURCE_UNSUPPORTED_ROOT: return "unsupported_root";
    case VPS_QUERY_SOURCE_FORBIDDEN_COMMAND: return "forbidden_command";
    case VPS_QUERY_SOURCE_DATA_MODIFYING_CTE: return "data_modifying_cte";
    case VPS_QUERY_SOURCE_UNRESOLVED_PARAMETER: return "unresolved_parameter";
    case VPS_QUERY_SOURCE_LOCKING_SELECT: return "locking_select";
    case VPS_QUERY_SOURCE_SELECT_INTO: return "select_into";
    case VPS_QUERY_SOURCE_UNBALANCED: return "unbalanced";
    default: return "unknown";
    }
}

const char *vps_query_source_root_name(VpsQuerySourceRoot root)
{
    switch (root) {
    case VPS_QUERY_ROOT_SELECT: return "select";
    case VPS_QUERY_ROOT_WITH_SELECT: return "with_select";
    case VPS_QUERY_ROOT_NONE:
    default: return "none";
    }
}
