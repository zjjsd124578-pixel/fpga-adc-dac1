#include "dds_command.h"

static int is_space(char c)
{
    return c == ' ' || c == '\t';
}
static int is_end(char c)
{
    return is_space(c) || c == '\r' || c == '\n' || c == ';';
}
int DdsCommand_Parse(const char *text, size_t length, DdsCommand *command)
{
    size_t i = 0U;
    uint32_t value = 0U, digit;
    char code;
    int digits = 0;
    if (text == NULL || command == NULL) return DDS_COMMAND_INVALID;
    while (i < length && is_space(text[i])) ++i;
    if (i == length) return DDS_COMMAND_OTHER;
    code = text[i++];
    if (code >= 'a' && code <= 'z') code = (char)(code - 'a' + 'A');
    if (code != 'W' && code != 'F' && code != 'P' && code != 'E' &&
        code != 'S' && code != 'B' && code != 'C' && code != 'D' && code != 'A')
        return DDS_COMMAND_OTHER;
    /* Avoid mistaking WINDOW, PAUSE, DUMP etc. for a single-letter command. */
    if (i < length && ((text[i] >= 'A' && text[i] <= 'Z') ||
                       (text[i] >= 'a' && text[i] <= 'z')))
        return DDS_COMMAND_OTHER;
    while (i < length && is_space(text[i])) ++i;
    if (i < length && (text[i] == '=' || text[i] == ':')) ++i;
    while (i < length && is_space(text[i])) ++i;
    while (i < length && text[i] >= '0' && text[i] <= '9') {
        digit = (uint32_t)(text[i++] - '0');
        if (value > (UINT32_MAX - digit) / 10U) return DDS_COMMAND_INVALID;
        value = value * 10U + digit;
        digits = 1;
    }
    if (!digits) return DDS_COMMAND_INVALID;
    while (i < length && is_end(text[i])) ++i;
    if (i != length) return DDS_COMMAND_INVALID;
    if (code == 'W' && (value < 1U || value > 5U)) return DDS_COMMAND_INVALID;
    if ((code == 'E' || code == 'S') && value > 1U) return DDS_COMMAND_INVALID;
    if (code == 'A' && value > 1000U) return DDS_COMMAND_INVALID;
    command->code = code;
    command->value = value;
    return DDS_COMMAND_VALID;
}
