#include "sim_flash.h"
#include <stdio.h>
#include <string.h>

static int fault_hit(sim_fault_t *f, uint32_t *cnt, uint32_t hit_at)
{
    (void)f;
    (*cnt)++;
    if (hit_at != 0 && *cnt == hit_at) return 1;
    return 0;
}

void sim_flash_set_fault_ctx(sim_flash_t *f, fault_ctx_t *ctx)
{
    if (!f) return;
    f->fault_ctx = ctx;
}

int sim_flash_init(sim_flash_t *f, uint8_t *buf, uint32_t size,
                   uint32_t sector_size, uint32_t page_size,
                   uint8_t write_gran)
{
    if (!f || !buf || size == 0 || sector_size == 0) return -1;
    if (sector_size & (sector_size - 1)) return -1;
    if (size % sector_size) return -1;
    if (write_gran > 3) return -1;

    f->mem = buf;
    f->size = size;
    f->sector_size = sector_size;
    f->page_size = page_size;
    f->write_gran = write_gran;
    f->fault_ctx = NULL;  /* 默认禁用高级故障注入 */
    sim_flash_reset_faults(f);
    memset(f->mem, 0xFF, f->size);
    return 0;
}

int sim_flash_read(sim_flash_t *f, uint32_t addr, uint8_t *buf, size_t len)
{
    if (!f || !buf || addr + len > f->size) return -1;

    /* 高级故障注入 */
    if (f->fault_ctx && fault_should_read_fail(f->fault_ctx, addr, len)) {
        return -1;
    }

    /* 旧的简单故障系统 */
    if (fault_hit(&f->old_fault, &f->old_fault.read_count, f->old_fault.read_fail_at))
        return -1;

    memcpy(buf, f->mem + addr, len);

    if (f->trace) trace_flash_read(f->trace, addr, buf, len);

    return 0;
}

int sim_flash_write(sim_flash_t *f, uint32_t addr, const uint8_t *buf, size_t len)
{
    if (!f || !buf || addr + len > f->size) return -1;

    uint32_t g = 1u << f->write_gran;
    /* Single-byte writes (commit byte, state transitions) are always
       permitted — real NOR flash supports byte-program within a word. */
    if (len != 1u) {
        if ((addr % g) != 0 || (len % g) != 0) return -1;
    }

    /* 高级故障注入 */
    if (f->fault_ctx && fault_should_write_fail(f->fault_ctx, addr, len)) {
        return -1;
    }

    /* 旧的简单故障系统 */
    if (fault_hit(&f->old_fault, &f->old_fault.write_count, f->old_fault.write_fail_at))
        return -1;

    /* Save before-image for tracing */
    uint8_t before[512];
    size_t trace_len = len < sizeof(before) ? len : sizeof(before);
    if (f->trace && f->trace->level >= 3) {
        memcpy(before, f->mem + addr, trace_len);
    }

    for (size_t i = 0; i < len; i++) {
        uint8_t old = f->mem[addr + i];
        uint8_t nw  = buf[i];
        if ((~old) & nw) {
            /* Attempting to set 0->1 is illegal without erase */
            return -1;
        }

        /* Write byte first, then check for power loss.
           This allows partial writes: if power loss triggers at byte i,
           bytes 0..i-1 are already committed to flash. */
        f->mem[addr + i] &= nw;

        if (f->fault_ctx && fault_should_power_loss(f->fault_ctx, addr, (uint32_t)i)) {
            if (f->trace) trace_flash_write(f->trace, addr, before, f->mem + addr, trace_len);
            return -1;
        }
    }

    if (f->trace) trace_flash_write(f->trace, addr, before, f->mem + addr, trace_len);

    /* 注入数据损坏 */
    if (f->fault_ctx) {
        fault_inject_corruption(f->fault_ctx, f->mem, addr, (uint32_t)len);
    }

    return 0;
}

int sim_flash_erase(sim_flash_t *f, uint32_t addr)
{
    if (!f || addr >= f->size) return -1;
    if (addr % f->sector_size) return -1;

    /* 高级故障注入 */
    if (f->fault_ctx && fault_should_erase_fail(f->fault_ctx, addr)) {
        return -1;
    }

    /* 旧的简单故障系统 */
    if (fault_hit(&f->old_fault, &f->old_fault.erase_count, f->old_fault.erase_fail_at))
        return -1;

    /* Trace: save a sample of pre-erase data */
    if (f->trace && f->trace->level >= 3) {
        /* Snapshot first 64 bytes of sector before erase for trace */
        uint8_t sample[64];
        memcpy(sample, f->mem + addr, sizeof(sample));
        trace_flash_erase(f->trace, addr, f->sector_size);
        /* Show the pre-erase sample */
        if (f->trace->fp) {
            fprintf(f->trace->fp, "  pre-erase sample (first 64B):\n");
            trace_hex_dump(f->trace->fp, sample, sizeof(sample), addr);
        }
    }

    memset(f->mem + addr, 0xFF, f->sector_size);

    if (f->trace && f->trace->level < 3)
        trace_flash_erase(f->trace, addr, f->sector_size);

    return 0;
}

void sim_flash_reset_faults(sim_flash_t *f)
{
    if (!f) return;
    f->old_fault.read_fail_at = 0;
    f->old_fault.write_fail_at = 0;
    f->old_fault.erase_fail_at = 0;
    f->old_fault.read_count = 0;
    f->old_fault.write_count = 0;
    f->old_fault.erase_count = 0;
}

void sim_flash_set_fault(sim_flash_t *f, uint32_t r, uint32_t w, uint32_t e)
{
    if (!f) return;
    f->old_fault.read_fail_at = r;
    f->old_fault.write_fail_at = w;
    f->old_fault.erase_fail_at = e;
}

int sim_flash_save_file(const sim_flash_t *f, const char *path)
{
    if (!f || !f->mem || !path) return -1;

    FILE *fp = fopen(path, "wb");
    if (!fp) return -1;
    size_t wrote = fwrite(f->mem, 1u, f->size, fp);
    int close_rc = fclose(fp);
    return (wrote == f->size && close_rc == 0) ? 0 : -1;
}

int sim_flash_load_file(sim_flash_t *f, const char *path)
{
    if (!f || !f->mem || !path) return -1;

    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;
    size_t read_n = fread(f->mem, 1u, f->size, fp);
    int extra = fgetc(fp);
    int close_rc = fclose(fp);
    return (read_n == f->size && extra == EOF && close_rc == 0) ? 0 : -1;
}
