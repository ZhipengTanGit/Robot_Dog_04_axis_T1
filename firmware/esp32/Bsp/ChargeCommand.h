#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef struct {
    char id[33], status[12], reason[40];
    unsigned target_ma, applied_ma;
} ChargeCommandAck;
bool ChargeCommand_Allowed(unsigned ma);
bool ChargeCommand_IdValid(const char *id);
bool ChargeCommand_ParseAck(const char *line, ChargeCommandAck *out);
size_t ChargeCommand_Format(char *buffer, size_t size, unsigned ma, const char *id);
