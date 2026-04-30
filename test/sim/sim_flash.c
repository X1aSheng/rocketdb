#include "sim_flash.h"
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
    return 0;
}

int sim_flash_write(sim_flash_t *f, uint32_t addr, const uint8_t *buf, size_t len)
{
    if (!f || !buf || addr + len > f->size) return -1;

    uint32_t g = 1u << f->write_gran;
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
            return -1;
        }
    }
    
    /* 注入数据损坏 */
    if (f->fault_ctx) {
        fault_inject_corruption(f->fault_ctx, f->mem, addr, len);
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

    memset(f->mem + addr, 0xFF, f->sector_size);
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
