/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SR_AUDIO_FRAMES_H
#define SR_AUDIO_FRAMES_H
#include <stddef.h>
/* Find the closest complete, corroborated ADTS frame boundary. */
int sr_adts_boundary(const unsigned char *data, size_t size, size_t near, size_t *boundary);
#endif
