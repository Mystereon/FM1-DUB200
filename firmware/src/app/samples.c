/* SPDX-License-Identifier: GPL-3.0-only */
/* DUB-200 sample bank. A slot is independent, erased and committed on its
 * own. PCM is never copied to RAM: valid payloads are read through the plain
 * XIP window by the DSP. USB only leaves a short frame here; the main loop
 * performs every flash write while the transport is silent.
 *
 * SysEx wire protocol (all messages are F0 7D 44 CMD pack7(PAYLOAD) F7):
 *   01 BEGIN  slot:u8 bytes:u32 crc32:u32 name:16 ASCII bytes
 *   02 DATA   slot:u8 offset:u32 pcm8 bytes (<= 40 bytes/frame)
 *   03 COMMIT slot:u8
 *   04 CLEAR  slot:u8
 *   05 QUERY  slot:u8 (255 = all)
 * Reply: F0 7D 44 70 CMD STATUS SLOT F7, STATUS 0 = accepted.
 *
 * User files are converted to mono unsigned PCM8 at 11,025 Hz. The
 * converter in tools/dub200_samples.py produces this transport format. */
#include "samples.h"

#define SMP_MAGIC 0x30325053u                 /* "SP20" */
#define SMP_FORMAT 1u
#define SMP_BASE 0x000A0000u
#define SMP_WIRE_MAX 96u
#define SMP_DEC_MAX 64u
#define SMP_DATA_FRAME_MAX 40u
#define SMP_BARRIER() __asm__ volatile("" ::: "memory")

typedef struct {
    uint32_t magic;
    uint16_t format;
    uint8_t slot;
    uint8_t rsv;
    uint32_t bytes;
    uint32_t crc;
    char name[16];
    uint32_t hcrc;
} smp_head_t;
_Static_assert(sizeof(smp_head_t) == 36u, "sample header changed");

static const uint8_t *smp_data[EN_SAMPLE_SLOTS];
static uint32_t smp_bytes[EN_SAMPLE_SLOTS];
static char smp_name[EN_SAMPLE_SLOTS][17];
static struct {
    uint8_t active, slot;
    uint32_t bytes, received, crc;
    char name[16];
} smp_upload;
static uint8_t smp_wire[SMP_WIRE_MAX];
static volatile uint8_t smp_wire_len, smp_wire_ready;

static uint32_t smp_crc32(const void *p, uint32_t n)
{
    static const uint32_t T[16] = {
        0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
        0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu, 0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu};
    const uint8_t *b = p;
    uint32_t c = 0xFFFFFFFFu;
    while (n--) {
        c ^= *b++;
        c = (c >> 4) ^ T[c & 15u];
        c = (c >> 4) ^ T[c & 15u];
    }
    return ~c;
}

static uint32_t smp_rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint32_t smp_unpack7(const uint8_t *in, uint32_t n, uint8_t *out, uint32_t max)
{
    uint32_t acc = 0, nb = 0, o = 0;
    while (n--) {
        acc |= (uint32_t)(*in++ & 0x7Fu) << nb;
        nb += 7u;
        while (nb >= 8u) {
            if (o < max)
                out[o] = (uint8_t)acc;
            o++;
            acc >>= 8u;
            nb -= 8u;
        }
    }
    return o;
}

static void smp_set_label(unsigned slot, const char *name)
{
    unsigned i;
    if (slot >= EN_SAMPLE_SLOTS)
        return;
    for (i = 0; i < 16u && name[i]; i++)
        smp_name[slot][i] = name[i];
    smp_name[slot][i] = 0;
    if (!i) {
        smp_name[slot][0] = 'S';
        smp_name[slot][1] = (char)('0' + slot / 2u + 1u);
        smp_name[slot][2] = slot & 1u ? 'B' : 'A';
        smp_name[slot][3] = 0;
    }
}

static void smp_publish(unsigned slot, const uint8_t *data, uint32_t n, const char *name)
{
    if (slot >= EN_SAMPLE_SLOTS)
        return;
    smp_data[slot] = data;
    smp_bytes[slot] = n;
    smp_set_label(slot, name);
    /* Asset pointers only change at boot or after smp_quiet() has stopped all
     * sample voices; en_sample_asset consequently never races a live reader. */
    en_sample_asset((int)slot, data, n);
}

uint8_t samples_mode_clamp(int mode)
{
    return (uint8_t)(mode < EN_SAMPLE_OFF || mode >= EN_SAMPLE_MODES ? EN_SAMPLE_OFF : mode);
}

const char *samples_mode_name(int mode)
{
    static const char *const N[EN_SAMPLE_MODES] = {"OFF", "A", "B", "ALT"};
    return N[samples_mode_clamp(mode)];
}

int samples_present(unsigned slot) { return slot < EN_SAMPLE_SLOTS && smp_bytes[slot] != 0u; }
const char *samples_name(unsigned slot) { return slot < EN_SAMPLE_SLOTS ? smp_name[slot] : ""; }

void samples_apply_modes(const uint8_t mode[EN_SAMPLE_LAYERS])
{
    unsigned i;
    for (i = 0; i < EN_SAMPLE_LAYERS; i++)
        en_sample_mode((int)i, mode ? samples_mode_clamp(mode[i]) : EN_SAMPLE_OFF);
}

#ifdef EN_HOST
static uint8_t smp_host[EN_SAMPLE_SLOTS][EN_SAMPLE_MAX_BYTES];

static int smp_erase_slot(unsigned slot)
{
    uint32_t i;
    if (slot >= EN_SAMPLE_SLOTS)
        return -1;
    smp_upload.active = 0;
    for (i = 0; i < EN_SAMPLE_MAX_BYTES; i++)
        smp_host[slot][i] = 0x80u;
    smp_publish(slot, 0, 0, "");
    return 0;
}

static int smp_begin(unsigned slot, uint32_t n, uint32_t crc, const char *name)
{
    if (slot >= EN_SAMPLE_SLOTS || !n || n > EN_SAMPLE_MAX_BYTES)
        return -1;
    smp_erase_slot(slot);
    smp_upload.active = 1;
    smp_upload.slot = (uint8_t)slot;
    smp_upload.bytes = n;
    smp_upload.received = 0;
    smp_upload.crc = crc;
    for (unsigned i = 0; i < sizeof smp_upload.name; i++)
        smp_upload.name[i] = name[i];
    return 0;
}

static int smp_write_data(unsigned slot, uint32_t off, const uint8_t *p, uint32_t n)
{
    uint32_t i;
    if (!smp_upload.active || slot != smp_upload.slot || off != smp_upload.received || !n || n > SMP_DATA_FRAME_MAX || off + n > smp_upload.bytes)
        return -1;
    for (i = 0; i < n; i++)
        smp_host[slot][off + i] = p[i];
    smp_upload.received += n;
    return 0;
}

static int smp_commit(unsigned slot)
{
    if (!smp_upload.active || slot != smp_upload.slot || smp_upload.received != smp_upload.bytes ||
        smp_crc32(smp_host[slot], smp_upload.bytes) != smp_upload.crc)
        return -1;
    smp_publish(slot, smp_host[slot], smp_upload.bytes, smp_upload.name);
    smp_upload.active = 0;
    return 0;
}

int samples_test_write(unsigned slot, const uint8_t *pcm, uint32_t n, const char *name)
{
    uint32_t i;
    if (slot >= EN_SAMPLE_SLOTS || !pcm || !n || n > EN_SAMPLE_MAX_BYTES)
        return -1;
    for (i = 0; i < n; i++)
        smp_host[slot][i] = pcm[i];
    smp_publish(slot, smp_host[slot], n, name ? name : "");
    return 0;
}

void samples_init(void)
{
    unsigned i;
    for (i = 0; i < EN_SAMPLE_SLOTS; i++)
        smp_publish(i, 0, 0, "");
}

#else

static uint32_t smp_base(unsigned slot) { return SMP_BASE + slot * EN_SAMPLE_SLOT_BYTES; }

static void smp_quiet(void)
{
    en_play(0);
    en_panic();
    /* The queued panic is drained by one 5.8 ms audio block before any XIP
     * bytes are erased. The transport is intentionally unavailable during an
     * import rather than risking reads from a sector being erased. */
    fm1_delay_ms(30);
}

static int smp_erase_slot(unsigned slot)
{
    uint32_t off;
    if (!flash_ok || slot >= EN_SAMPLE_SLOTS)
        return -1;
    smp_upload.active = 0;
    smp_quiet();
    for (off = 0; off < EN_SAMPLE_SLOT_BYTES; off += 0x1000u)
        if (st_erase(smp_base(slot) + off))
            return -1;
    smp_publish(slot, 0, 0, "");
    return 0;
}

static int smp_begin(unsigned slot, uint32_t n, uint32_t crc, const char *name)
{
    unsigned i;
    if (slot >= EN_SAMPLE_SLOTS || !n || n > EN_SAMPLE_MAX_BYTES || smp_erase_slot(slot))
        return -1;
    smp_upload.active = 1;
    smp_upload.slot = (uint8_t)slot;
    smp_upload.bytes = n;
    smp_upload.received = 0;
    smp_upload.crc = crc;
    for (i = 0; i < sizeof smp_upload.name; i++)
        smp_upload.name[i] = name[i];
    return 0;
}

static int smp_write_data(unsigned slot, uint32_t off, const uint8_t *p, uint32_t n)
{
    if (!smp_upload.active || slot != smp_upload.slot || off != smp_upload.received || !n || n > SMP_DATA_FRAME_MAX ||
        off + n > smp_upload.bytes || st_prog(smp_base(slot) + EN_SAMPLE_HEADER_BYTES + off, p, n))
        return -1;
    smp_upload.received += n;
    return 0;
}

static int smp_commit(unsigned slot)
{
    smp_head_t h;
    unsigned i;
    const uint8_t *data;
    if (!smp_upload.active || slot != smp_upload.slot || smp_upload.received != smp_upload.bytes)
        return -1;
    data = fm1_xip_ptr(smp_base(slot) + EN_SAMPLE_HEADER_BYTES);
    if (smp_crc32(data, smp_upload.bytes) != smp_upload.crc)
        return -1;
    h.magic = SMP_MAGIC;
    h.format = SMP_FORMAT;
    h.slot = (uint8_t)slot;
    h.rsv = 0xFFu;
    h.bytes = smp_upload.bytes;
    h.crc = smp_upload.crc;
    for (i = 0; i < sizeof h.name; i++)
        h.name[i] = smp_upload.name[i];
    h.hcrc = smp_crc32(&h, (uint32_t)((const uint8_t *)&h.hcrc - (const uint8_t *)&h));
    /* The metadata is written first with its magic still erased; the magic is
     * the final commit word. A power loss cannot yield a valid new header. */
    if (st_prog(smp_base(slot) + 4u, (const uint8_t *)&h + 4u, sizeof h - 4u) || st_prog(smp_base(slot), &h.magic, 4u))
        return -1;
    smp_publish(slot, data, h.bytes, h.name);
    smp_upload.active = 0;
    return 0;
}

void samples_init(void)
{
    unsigned slot;
    if (flash_ok)
        fl_plain_window_init();
    for (slot = 0; slot < EN_SAMPLE_SLOTS; slot++) {
        smp_head_t h;
        const uint8_t *data = 0;
        uint32_t n = 0;
        if (flash_ok && !st_read(smp_base(slot), &h, sizeof h) && h.magic == SMP_MAGIC && h.format == SMP_FORMAT &&
            h.slot == slot && h.bytes > 0u && h.bytes <= EN_SAMPLE_MAX_BYTES &&
            h.hcrc == smp_crc32(&h, (uint32_t)((const uint8_t *)&h.hcrc - (const uint8_t *)&h))) {
            data = fm1_xip_ptr(smp_base(slot) + EN_SAMPLE_HEADER_BYTES);
            n = h.bytes;
        }
        smp_publish(slot, data, n, n ? h.name : "");
    }
}
#endif

static void smp_reply(uint8_t cmd, uint8_t status, uint8_t slot)
{
#ifndef EN_HOST
    uint8_t p[8] = {0xF0u, 0x7Du, 0x44u, 0x70u, cmd, status, slot, 0xF7u};
    (void)ota_wire_send(p, sizeof p);
#else
    (void)cmd;
    (void)status;
    (void)slot;
#endif
}

int samples_wire_offer(const uint8_t *frame, uint32_t n)
{
    uint32_t i;
    if (!frame || n < 5u || n > SMP_WIRE_MAX || frame[0] != 0xF0u || frame[1] != 0x7Du || frame[2] != 0x44u ||
        frame[n - 1u] != 0xF7u)
        return 0;
    if (smp_wire_ready)
        return 1;                                  /* sender retries after its previous reply */
    for (i = 0; i < n; i++)
        smp_wire[i] = frame[i];
    smp_wire_len = (uint8_t)n;
    SMP_BARRIER();
    smp_wire_ready = 1;
    return 1;
}

void samples_service(void)
{
    uint8_t frame[SMP_WIRE_MAX], dec[SMP_DEC_MAX], cmd, slot = 0x7Fu, status = 1;
    uint32_t n, dn, i;
    if (!smp_wire_ready)
        return;
    SMP_BARRIER();
    n = smp_wire_len;
    for (i = 0; i < n; i++)
        frame[i] = smp_wire[i];
    SMP_BARRIER();
    smp_wire_ready = 0;
    cmd = frame[3];
    dn = smp_unpack7(frame + 4u, n - 5u, dec, sizeof dec);
    if (dn > sizeof dec)
        goto out;
    if (cmd == 1u && dn == 25u) {
        slot = dec[0];
        status = (uint8_t)(smp_begin(slot, smp_rd32(dec + 1u), smp_rd32(dec + 5u), (const char *)dec + 9u) != 0);
    } else if (cmd == 2u && dn >= 6u) {
        slot = dec[0];
        status = (uint8_t)(smp_write_data(slot, smp_rd32(dec + 1u), dec + 5u, dn - 5u) != 0);
    } else if (cmd == 3u && dn == 1u) {
        slot = dec[0];
        status = (uint8_t)(smp_commit(slot) != 0);
    } else if (cmd == 4u && dn == 1u) {
        slot = dec[0];
        status = (uint8_t)(smp_erase_slot(slot) != 0);
    } else if (cmd == 5u && dn == 1u) {
        slot = dec[0];
        status = slot < EN_SAMPLE_SLOTS || slot == 0xFFu ? 0u : 1u;
    }
out:
    smp_reply(cmd, status, slot);
}
