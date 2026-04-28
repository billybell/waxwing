#ifndef WAXWING_COMMANDS_H
#define WAXWING_COMMANDS_H

#include <stdint.h>
#include <stddef.h>

/**
 * Dispatch a CBOR file command. Parses incoming CBOR map, calls filestore,
 * and encodes response into out_buf.
 *
 * Returns bytes written to out_buf (0-512), or -1 on error.
 */
int commands_handle(const uint8_t *cmd_data, size_t cmd_len,
                    uint8_t *out_buf, size_t out_max);

#endif // WAXWING_COMMANDS_H
