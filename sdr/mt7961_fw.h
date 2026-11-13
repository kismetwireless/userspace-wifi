#ifndef MT7961_FW_H
#define MT7961_FW_H

#include <stddef.h>
#include <stdint.h>

/* the MT7921AU's WM firmware, and the I/Q stream patch to it */

#define MT7961_FW_OTHER 0
#define MT7961_FW_STOCK 1
#define MT7961_FW_PATCHED 2

int mt7961_fw_state(const uint8_t *data, size_t len);
/* stock firmware to the I/Q build, in place; -1 for anything else */
int mt7961_fw_patch(uint8_t *data, size_t len);
/* a userspace firmware loader filter that patches the stock WM firmware */
int mt7961_fw_filter(const char *name, uint8_t **data, size_t *len);

#endif
