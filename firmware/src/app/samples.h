/* SPDX-License-Identifier: GPL-3.0-only */
/* DUB-200's user-sample bank. The ten assets are intentionally separate from
 * projects: projects only retain an Off/A/B/Alt choice for each of DUB-1's five
 * layers. The PCM data lives in an unused, update-safe part of the FM-1 flash. */
#pragma once
#include <stdint.h>
#include "../dsp/amb.h"

#define EN_SAMPLE_RATE 11025u
#define EN_SAMPLE_SLOT_BYTES 0x6000u
#define EN_SAMPLE_HEADER_BYTES 256u
#define EN_SAMPLE_MAX_BYTES (EN_SAMPLE_SLOT_BYTES - EN_SAMPLE_HEADER_BYTES)

/* Called once after en_init, before the transport is enabled. It maps valid
 * committed assets into the DSP. Empty or interrupted slots are silent. */
void samples_init(void);

/* Apply a piece's five Off/A/B/Alt choices to the DSP. */
void samples_apply_modes(const uint8_t mode[EN_SAMPLE_LAYERS]);
uint8_t samples_mode_clamp(int mode);
const char *samples_mode_name(int mode);
int samples_present(unsigned slot);
const char *samples_name(unsigned slot);

/* USB MIDI SysEx receiver: called from usb.c only for F0 7D 44 ... F7 frames;
 * it copies one short frame and returns immediately. samples_service() runs
 * the flash work in the main loop, never in the audio/USB interrupt. */
int samples_wire_offer(const uint8_t *frame, uint32_t n);
void samples_service(void);

#ifdef EN_HOST
/* Test hook: materialise one legal PCM8 asset in the host-only flash model. */
int samples_test_write(unsigned slot, const uint8_t *pcm, uint32_t n, const char *name);
#endif
