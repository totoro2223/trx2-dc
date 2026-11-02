#include <trx/core/strings/fuzzy_match.h>

#include <trx/core/memory.h>
#include <trx/core/strings/common.h>

#include <ctype.h>
#include <string.h>
#include <strings.h>

#define FULL_MATCH_SCORE_BONUS 100
#define WORD_MATCH_SCORE_BONUS 50
#define PERCENT_MATCH_SCORE 50
#define LETTER_MATCH_SCORE_BONUS 1

#if defined(__DREAMCAST__)
static bool M_IsWordChar(const char ch)
{
    return isalnum((unsigned char)ch) != 0 || ch == '_';
}

// "^\s*input\s*$", ignoring case.
static bool M_IsFullMatch(const char *reference, const char *const input)
{
    while (isspace((unsigned char)*reference)) {
        reference++;
    }
    const size_t len = strlen(input);
    if (strncasecmp(reference, input, len) != 0) {
        return false;
    }
    for (const char *p = reference + len; *p != '\0'; p++) {
        if (!isspace((unsigned char)*p)) {
            return false;
        }
    }
    return true;
}

// "\binput\b", ignoring case.
static bool M_IsWordMatch(const char *const reference, const char *const input)
{
    const size_t len = strlen(input);
    if (len == 0) {
        return false;
    }
    for (const char *p = reference; *p != '\0'; p++) {
        if (strncasecmp(p, input, len) != 0) {
            continue;
        }
        const bool starts =
            p == reference || M_IsWordChar(p[-1]) != M_IsWordChar(p[0]);
        const bool ends = p[len] == '\0'
            || M_IsWordChar(p[len - 1]) != M_IsWordChar(p[len]);
        if (starts && ends) {
            return true;
        }
    }
    return false;
}
#endif

#if !defined(__DREAMCAST__)
// Quotes the regular expression syntax in the input, so that a name holding a
// metacharacter is searched for rather than compiled. Bytes outside ASCII are
// left alone, because a backslash before a UTF-8 continuation byte does not
// compile.
static char *M_QuoteRegex(const char *const input)
{
    char *const result = Memory_Alloc(strlen(input) * 2 + 1);
    char *out = result;
    for (const char *p = input; *p != '\0'; p++) {
        const unsigned char ch = (unsigned char)*p;
        if (ch < 0x80 && isalnum(ch) == 0 && ch != '_') {
            *out++ = '\\';
        }
        *out++ = *p;
    }
    *out = '\0';
    return result;
}

#endif

static STRING_FUZZY_SCORE M_GetScore(
    const char *const user_input, const char *const reference,
    const int32_t weight)
{
    const size_t reference_len = strlen(reference);
    const int32_t percent_score = reference_len == 0
        ? 0
        : PERCENT_MATCH_SCORE * strlen(user_input) / reference_len;
    const int32_t letter_score = LETTER_MATCH_SCORE_BONUS * strlen(user_input);

#if defined(__DREAMCAST__)
    // What the two expressions below match, without a regex engine (none on
    // the Dreamcast): the whole reference but for spaces either side, and
    // the input as a word of it.
    const bool full = M_IsFullMatch(reference, user_input);
    const bool word = !full && M_IsWordMatch(reference, user_input);
#else
    char *quoted = M_QuoteRegex(user_input);
    char *word_regex = String_Format("\\b%s\\b", quoted);
    char *full_regex = String_Format("^\\s*%s\\s*$", quoted);
    Memory_FreePointer(&quoted);
    const bool full = String_Match(reference, full_regex);
    const bool word = !full && String_Match(reference, word_regex);
    Memory_FreePointer(&word_regex);
    Memory_FreePointer(&full_regex);
#endif

    // Assume a partial match
    bool is_full = false;
    bool is_word = false;
    int32_t score = letter_score + percent_score;
    if (full) {
        // Got a full match
        is_full = true;
        score += FULL_MATCH_SCORE_BONUS;
    } else if (word) {
        // Got a word match
        is_word = true;
        score += WORD_MATCH_SCORE_BONUS;
    } else if (String_CaseSubstring(reference, user_input) == nullptr) {
        // No match.
        score = 0;
    }

    return (STRING_FUZZY_SCORE) {
        .is_full = is_full,
        .is_word = is_word,
        .score = score * weight,
    };
}

static void M_DiscardNonFullMatches(VECTOR *const matches)
{
    bool has_full_match = false;
    for (int32_t i = 0; i < matches->count; i++) {
        const STRING_FUZZY_MATCH *const match = Vector_Get(matches, i);
        if (match->score.is_full) {
            has_full_match = true;
        }
    }
    if (has_full_match) {
        for (int32_t i = matches->count - 1; i >= 0; i--) {
            const STRING_FUZZY_MATCH *const match = Vector_Get(matches, i);
            if (!match->score.is_full) {
                Vector_RemoveAt(matches, i);
            }
        }
    }
}

static void M_DiscardNonWordMatches(VECTOR *const matches)
{
    bool has_word_match = false;
    for (int32_t i = 0; i < matches->count; i++) {
        const STRING_FUZZY_MATCH *const match = Vector_Get(matches, i);
        if (match->score.is_word) {
            has_word_match = true;
        }
    }
    if (has_word_match) {
        for (int32_t i = matches->count - 1; i >= 0; i--) {
            const STRING_FUZZY_MATCH *const match = Vector_Get(matches, i);
            if (!match->score.is_word) {
                Vector_RemoveAt(matches, i);
            }
        }
    }
}

static void M_SortMatches(VECTOR *const matches)
{
    // sort by match length so that best-matching results appear first
    for (int32_t i = 0; i < matches->count; i++) {
        const STRING_FUZZY_MATCH *const match_1 = Vector_Get(matches, i);
        for (int32_t j = i + 1; j < matches->count; j++) {
            const STRING_FUZZY_MATCH *const match_2 = Vector_Get(matches, j);
            if (match_1->score.score < match_2->score.score) {
                Vector_Swap(matches, i, j);
            }
        }
    }
}

static void M_DiscardDuplicateMatches(VECTOR *const matches)
{
    for (int32_t i = matches->count - 1; i >= 0; i--) {
        const STRING_FUZZY_MATCH *const match = Vector_Get(matches, i);
        bool is_unique = true;
        for (int32_t j = 0; j < matches->count; j++) {
            const STRING_FUZZY_MATCH *const other_match =
                Vector_Get(matches, j);
            if (j != i && match->value == other_match->value) {
                is_unique = false;
                break;
            }
        }
        if (!is_unique) {
            Vector_RemoveAt(matches, i);
        }
    }
}

VECTOR *String_FuzzyMatch(const char *user_input, const VECTOR *const source)
{
    VECTOR *matches = Vector_Create(sizeof(STRING_FUZZY_MATCH));

    for (int32_t i = 0; i < source->count; i++) {
        const STRING_FUZZY_SOURCE *const source_item =
            Vector_Get((VECTOR *)source, i);
        const STRING_FUZZY_SCORE score =
            M_GetScore(user_input, source_item->key, source_item->weight);

        if (score.score <= 0) {
            continue;
        }

        STRING_FUZZY_MATCH match = {
            .key = source_item->key,
            .value = source_item->value,
            .score = score,
        };
        Vector_Add(matches, &match);
    }

    M_DiscardNonFullMatches(matches);
    M_DiscardNonWordMatches(matches);
    M_DiscardDuplicateMatches(matches);
    M_SortMatches(matches);

    return matches;
}
