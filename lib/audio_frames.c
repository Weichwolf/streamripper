/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "audio_frames.h"
#include <stdint.h>

static size_t frame_size(const unsigned char *data, size_t available)
{
    if (available < 7 || data[0] != 0xff || (data[1] & 0xf6U) != 0xf0 ||
        ((data[2] >> 2) & 15U) == 15)
        return 0;
    size_t size = ((size_t)(data[3] & 3U) << 11) | ((size_t)data[4] << 3) | (data[5] >> 5);
    return size >= (data[1] & 1U ? 7U : 9U) ? size : 0;
}

int sr_adts_boundary(const unsigned char *data, size_t size, size_t near, size_t *boundary)
{
    if (!data || !boundary)
        return 0;
    size_t best_distance = SIZE_MAX;
    for (size_t i = 1; i + 7 <= size; ++i) {
        size_t length = frame_size(data + i, size - i);
        if (!length || length > size - i)
            continue;
        size_t next = i + length;
        if (next != size && !frame_size(data + next, size - next))
            continue;
        size_t distance = i > near ? i - near : near - i;
        if (distance < best_distance) {
            best_distance = distance;
            *boundary = i;
        }
    }
    return best_distance != SIZE_MAX;
}
