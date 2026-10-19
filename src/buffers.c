/* The Buffer Manager: byte buffers programs and modules create (a
 * keyboard handler's, a device's), in RISC OS 3's ROM.
 *
 * The SWIs (&42940 on), as the PRMs give them:
 *   Buffer_Create (R0 flags, R1 size, R2 the handle wanted or -1 -> R0 the
 *     handle), Buffer_Remove (R0), Buffer_Register (R0 flags, R1/R2 the
 *     buffer's memory, R3 handle -> R0), Buffer_Deregister (R0),
 *     Buffer_ModifyFlags (R0, R1 EOR, R2 AND -> R1 old, R2 new),
 *     Buffer_GetInfo (R0 -> R0 flags, R1/R2 start and end, R3 insert and
 *     R4 remove offsets, R5 free, R6 used), Buffer_Threshold (R0, R1 -> R1
 *     the old), Buffer_LinkDevice/UnlinkDevice and Buffer_InternalInfo
 *     (accepted, nothing kept)
 * and the OS_Bytes on them: 138 and 153 insert R2 (C set if full), 145
 * remove into R2 and 152 look at the next (C set if empty), 21 flush.
 * Handles 0-9 are the kernel's own buffers (the keyboard's is elsewhere);
 * these start at 10. A buffer created here lives in an RMA block. */
#include <string.h>
#include "internal.h"

#define MAXBUF 16
static struct { uint32_t handle, mem, size, flags, threshold, in, out, used, owned; } buf[MAXBUF];

static int find(uint32_t h)
{
    for (int k = 0; k < MAXBUF; k++) if (buf[k].handle && buf[k].handle == h) return k;
    return -1;
}

static int add(uint32_t want, uint32_t mem, uint32_t size, uint32_t flags, int owned)
{
    int k;
    for (k = 0; k < MAXBUF && buf[k].handle; k++) ;
    if (k == MAXBUF) { os_error(0x20700, "No more buffers"); return -1; }
    uint32_t h = want;
    if ((int32_t)want < 0) for (h = 10; find(h) >= 0; h++) ;
    else if (find(h) >= 0) { os_error(0x20701, "Buffer handle in use"); return -1; }
    buf[k] = (typeof(buf[0])){h, mem, size, flags, 0, 0, 0, 0, (uint32_t)owned};
    return k;
}

static void drop(int k)
{
    if (buf[k].owned) { uint32_t s[3] = {R[0], R[1], R[2]}; R[0] = 7; R[2] = buf[k].mem; roger_swi(0x2001E); R[0] = s[0]; R[1] = s[1]; R[2] = s[2]; }
    buf[k].handle = 0;
}

int buffers_swi(uint32_t b)
{
    if (b < 0x42940 || b > 0x4297F) return 0;
    int k;
    switch (b - 0x42940) {
    case 0: {                                          /* Create */
        uint32_t flags = R[0], size = R[1], want = R[2];
        R[0] = 6; R[3] = size; roger_swi(0x2001E);
        if (VF) return 1;
        if ((k = add(want, R[2], size, flags, 1)) < 0) return 1;
        R[0] = buf[k].handle; R[1] = size; R[2] = want; R[3] = size;
        ev("Buffer_Create", "ii", buf[k].handle, size);
        return 1;
    }
    case 2:                                            /* Register */
        if ((k = add(R[3], R[1], R[2] - R[1], R[0], 0)) >= 0) R[0] = buf[k].handle;
        return 1;
    case 1: case 3:                                    /* Remove, Deregister */
        if ((k = find(R[0])) < 0) { os_error(0x20702, "Bad buffer handle"); return 1; }
        drop(k);
        return 1;
    case 4:                                            /* ModifyFlags */
        if ((k = find(R[0])) < 0) { os_error(0x20702, "Bad buffer handle"); return 1; }
        R[1] = buf[k].flags; buf[k].flags = (buf[k].flags & R[2]) ^ R[1]; R[2] = buf[k].flags;
        return 1;
    case 7:                                            /* GetInfo */
        if ((k = find(R[0])) < 0) { os_error(0x20702, "Bad buffer handle"); return 1; }
        R[0] = buf[k].flags; R[1] = buf[k].mem; R[2] = buf[k].mem + buf[k].size;
        R[3] = buf[k].in; R[4] = buf[k].out; R[5] = buf[k].size - buf[k].used; R[6] = buf[k].used;
        return 1;
    case 8:                                            /* Threshold */
        if ((k = find(R[0])) < 0) { os_error(0x20702, "Bad buffer handle"); return 1; }
        { uint32_t old = buf[k].threshold; if ((int32_t)R[1] >= 0) buf[k].threshold = R[1]; R[1] = old; }
        return 1;
    case 5: case 6: case 9:                            /* LinkDevice, UnlinkDevice, InternalInfo */
        return 1;
    }
    os_error(0x1E6, "SWI value out of range for module BufferManager");
    return 1;
}

/* OS_Byte 138, 145, 152, 153 and 21 on a buffer made here: 1 if handled */
int buffers_os_byte(uint32_t op)
{
    if (op != 138 && op != 145 && op != 152 && op != 153 && op != 21) return 0;
    int k = find(R[1] & 0xFF);
    if (k < 0) return 0;
    if (op == 21) { buf[k].in = buf[k].out = buf[k].used = 0; return 1; }
    if (op == 138 || op == 153) {
        CF = buf[k].used >= buf[k].size;
        if (CF) return 1;
        MEM[buf[k].mem + buf[k].in] = (uint8_t)R[2];
        buf[k].in = (buf[k].in + 1) % buf[k].size;
        buf[k].used++;
        return 1;
    }
    CF = buf[k].used == 0;                             /* 145 remove, 152 examine */
    if (CF) return 1;
    R[2] = MEM[buf[k].mem + buf[k].out];
    if (op == 145) { buf[k].out = (buf[k].out + 1) % buf[k].size; buf[k].used--; }
    return 1;
}
