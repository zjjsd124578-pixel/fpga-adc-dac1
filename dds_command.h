#ifndef DDS_COMMAND_H
#define DDS_COMMAND_H
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char code;
    uint32_t value;
} DdsCommand;

enum { DDS_COMMAND_INVALID = -1, DDS_COMMAND_OTHER = 0, DDS_COMMAND_VALID = 1 };
/* One complete ASCII command. Accept W1, W=1, W:1, W 1, and lowercase.
 * Optional trailing CR/LF/semicolon/whitespace. Values are decimal uint32_t.
 * Returns OTHER for existing scope commands such as WINDOW/ARM/PAUSE.
 * TCP is a byte stream: accumulate a full command before calling this function.
 */
int DdsCommand_Parse(const char *text, size_t length, DdsCommand *command);
#endif
