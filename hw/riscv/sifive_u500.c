/*
 * QEMU RISC-V Board Compatible with SiFive U500 SDK
 *
 * Copyright (c) 2016-2017 Sagar Karandikar, sagark@eecs.berkeley.edu
 * Copyright (c) 2017 SiFive, Inc.
 *
 * This provides a RISC-V Board with the following devices:
 *
 * 0) UART comptible with that expected by the SiFive U500 SDK
 *
 * This board currently uses a hardcoded devicetree that indicates one hart.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "hw/hw.h"
#include "hw/boards.h"
#include "hw/loader.h"
#include "hw/sysbus.h"
#include "hw/char/serial.h"
#include "hw/riscv/cpudevs.h"
#include "hw/riscv/htif/htif.h"
#include "hw/riscv/riscv_hart.h"
#include "hw/riscv/sifive_plic.h"
#include "hw/riscv/sifive_clint.h"
#include "hw/riscv/sifive_uart.h"
#include "hw/riscv/sifive_prci.h"
#include "hw/riscv/sifive_u500.h"
#include "sysemu/char.h"
#include "sysemu/arch_init.h"
#include "sysemu/device_tree.h"
#include "exec/address-spaces.h"
#include "elf.h"

const struct SiFiveMemmapEntry {
    hwaddr base;
    hwaddr size;    
} sifive_u500_memmap[] =
{
    [SIFIVE_U500_DEBUG] =    {        0x0,      0x100 },
    [SIFIVE_U500_MROM] =     {     0x1000,     0x2000 },
    [SIFIVE_U500_CLINT] =    {  0x2000000,    0x10000 },
    [SIFIVE_U500_PLIC] =     {  0xc000000,  0x4000000 },
    [SIFIVE_U500_UART0] =    { 0x10013000,     0x1000 },
    [SIFIVE_U500_UART1] =    { 0x10023000,     0x1000 },
};

static uint64_t identity_translate(void *opaque, uint64_t addr)
{
    return addr;
}

static uint64_t load_kernel(const char *kernel_filename)
{
    uint64_t kernel_entry, kernel_high;

    if (load_elf(kernel_filename, identity_translate, NULL,
                 &kernel_entry, NULL, &kernel_high,
                 /* little_endian = */ 0, ELF_MACHINE, 1, 0) < 0) {
        error_report("qemu: could not load kernel '%s'\n", kernel_filename);
        exit(1);
    }
    return kernel_entry;
}

static void create_fdt(SiFiveU500State *s, uint64_t mem_base, uint64_t mem_size)
{
    void *fdt;
    int cpu;

    fdt = s->fdt = create_device_tree(&s->fdt_size);
    if (!fdt) {
        error_report("create_device_tree() failed");
        exit(1);
    }

    qemu_fdt_setprop_string(fdt, "/", "model", "ucbbar,spike-bare,qemu");
    qemu_fdt_setprop_string(fdt, "/", "compatible", "ucbbar,spike-bare-dev");
    qemu_fdt_setprop_cell(fdt, "/", "#size-cells", 0x2);
    qemu_fdt_setprop_cell(fdt, "/", "#address-cells", 0x2);

    qemu_fdt_add_subnode(fdt, "/soc");
    qemu_fdt_setprop(fdt, "/soc", "ranges", NULL, 0);
    qemu_fdt_setprop_string(fdt, "/soc", "compatible", "ucbbar,spike-bare-soc");
    qemu_fdt_setprop_cell(fdt, "/soc", "#size-cells", 0x2);
    qemu_fdt_setprop_cell(fdt, "/soc", "#address-cells", 0x2);

    qemu_fdt_add_subnode(fdt, "/uart@10013000");
    qemu_fdt_setprop_string(fdt, "/uart@10013000",
        "compatible", "sifive,uart-1.0");
    qemu_fdt_setprop_cells(fdt, "/uart@10013000",
        "reg", 0x0, 0x10013000, 0x0, 0x20);

    qemu_fdt_add_subnode(fdt, "/soc/clint@2000000");
    qemu_fdt_setprop_string(fdt, "/soc/clint@2000000",
        "compatible", "riscv,clint0");
    qemu_fdt_setprop_cells(fdt, "/soc/clint@2000000",
        "reg", 0x0, 0x2000000, 0x0, 0xc0000);
    qemu_fdt_setprop_cells(fdt, "/soc/clint@2000000",
        "interrupts-extended", 1, 3, 1, 7);

    qemu_fdt_add_subnode(fdt, "/memory@80000000");
    qemu_fdt_setprop_cells(fdt, "/memory@80000000",
        "reg", mem_base >> 32, mem_base, mem_size >> 32, mem_size);
    qemu_fdt_setprop_string(fdt, "/memory@80000000",
        "device_type", "memory");

    qemu_fdt_add_subnode(fdt, "/cpus");
    qemu_fdt_setprop_cell(fdt, "/cpus", "timebase-frequency", 10000000);
    qemu_fdt_setprop_cell(fdt, "/cpus", "#size-cells", 0x0);
    qemu_fdt_setprop_cell(fdt, "/cpus", "#address-cells", 0x1);

    for (cpu = s->soc.num_harts - 1; cpu >= 0; cpu--) {
        char *nodename = g_strdup_printf("/cpus/cpu@%d", cpu);
        char *intc = g_strdup_printf("/cpus/cpu@%d/interrupt-controller", cpu);
        char* isa_string = riscv_isa_string(&s->soc.harts[cpu]);
        qemu_fdt_add_subnode(fdt, nodename);
        qemu_fdt_setprop_cell(fdt, nodename, "clock-frequency", 1000000000);
        qemu_fdt_setprop_string(fdt, nodename, "mmu-type", "riscv,sv48");
        qemu_fdt_setprop_string(fdt, nodename, "riscv,isa", isa_string);
        qemu_fdt_setprop_string(fdt, nodename, "compatible", "riscv");
        qemu_fdt_setprop_string(fdt, nodename, "status", "okay");
        qemu_fdt_setprop_cell(fdt, nodename, "reg", cpu);
        qemu_fdt_setprop_string(fdt, nodename, "device_type", "cpu");
        qemu_fdt_add_subnode(fdt, intc);
        qemu_fdt_setprop_cell(fdt, intc, "phandle", 1);
        qemu_fdt_setprop_cell(fdt, intc, "linux,phandle", 1);
        qemu_fdt_setprop_string(fdt, intc, "compatible", "riscv,cpu-intc");
        qemu_fdt_setprop(fdt, intc, "interrupt-controller", NULL, 0);
        qemu_fdt_setprop_cell(fdt, intc, "#interrupt-cells", 1);
        g_free(isa_string);
        g_free(nodename);
    }
}

static void riscv_sifive_u500_init(MachineState *machine)
{
    const struct SiFiveMemmapEntry *memmap = sifive_u500_memmap;

    SiFiveU500State *s = g_new0(SiFiveU500State, 1);
    MemoryRegion *system_memory = get_system_memory();
    MemoryRegion *main_mem = g_new(MemoryRegion, 1);
    MemoryRegion *boot_rom = g_new(MemoryRegion, 1);
    int i;

    /* Initialize SOC */
    object_initialize(&s->soc, sizeof(s->soc), TYPE_RISCV_HART_ARRAY);
    object_property_add_child(OBJECT(machine), "soc", OBJECT(&s->soc),
                              &error_abort);
    object_property_set_str(OBJECT(&s->soc), "riscv-imafdcs-priv1.10",
                            "cpu-model", &error_abort);
    object_property_set_int(OBJECT(&s->soc), smp_cpus, "num-harts",
                            &error_abort);
    object_property_set_bool(OBJECT(&s->soc), true, "realized",
                            &error_abort);

    /* Make sure the first 3 serial ports are associated with a device. */
    for (i = 0; i < 3; i++) {
        if (!serial_hds[i]) {
            char label[32];
            snprintf(label, sizeof(label), "serial%d", i);
            serial_hds[i] = qemu_chr_new(label, "null", NULL);
        }
    }

    /* register RAM */
    memory_region_init_ram(main_mem, NULL, "riscv.sifive.u500.ram",
                           machine->ram_size, &error_fatal);
    /* for phys mem size check in page table walk */
    vmstate_register_ram_global(main_mem);
    memory_region_add_subregion(system_memory, DRAM_BASE, main_mem);

    /* create device tree */
    create_fdt(s, DRAM_BASE, machine->ram_size);

    /* boot rom */
    memory_region_init_ram(boot_rom, NULL, "riscv.sifive.u500.bootrom",
                           0x10000, &error_fatal);
    vmstate_register_ram_global(boot_rom);
    memory_region_set_readonly(boot_rom, true);
    memory_region_add_subregion(system_memory, 0x0, boot_rom);

    if (machine->kernel_filename) {
         load_kernel(machine->kernel_filename);
    }

   /* reset vector */
    uint32_t reset_vec[8] = {
        0x00000297,                  /* 1:  auipc  t0, %pcrel_hi(dtb) */
        0x02028593,                  /*     addi   a1, t0, %pcrel_lo(1b) */
        0xf1402573,                  /*     csrr   a0, mhartid  */
#if defined(TARGET_RISCV32)
        0x0182a283,                  /*     lw     t0, 24(t0) */
#elif defined(TARGET_RISCV64)
        0x0182b283,                  /*     ld     t0, 24(t0) */
#endif
        0x00028067,                  /*     jr     t0 */
        0x00000000,
        DRAM_BASE,                   /* start: .dword DRAM_BASE */
        0x00000000,
                                     /* dtb: */
    };

    /* copy in the reset vector */
    cpu_physical_memory_write(memmap[SIFIVE_U500_MROM].base,
        reset_vec, sizeof(reset_vec));

    /* copy in the device tree */
    qemu_fdt_dumpdtb(s->fdt, s->fdt_size);
    cpu_physical_memory_write(memmap[SIFIVE_U500_MROM].base +
        sizeof(reset_vec), s->fdt, s->fdt_size);

    /* MMIO */
    s->plic = sifive_plic_create(memmap[SIFIVE_U500_PLIC].base, &s->soc,
        (char*)SIFIVE_U500_PLIC_HART_CONFIG,
        SIFIVE_U500_PLIC_NUM_SOURCES,
        SIFIVE_U500_PLIC_NUM_PRIORITIES,
        SIFIVE_U500_PLIC_PRIORITY_BASE,
        SIFIVE_U500_PLIC_PENDING_BASE,
        SIFIVE_U500_PLIC_ENABLE_BASE,
        SIFIVE_U500_PLIC_CLAIM_BASE,
        memmap[SIFIVE_U500_PLIC].size);
    sifive_uart_create(memmap[SIFIVE_U500_UART0].base, serial_hds[0],
        s->plic, SIFIVE_U500_UART0_IRQ);
    sifive_uart_create(memmap[SIFIVE_U500_UART1].base, serial_hds[0],
        s->plic, SIFIVE_U500_UART1_IRQ);
    sifive_clint_create(memmap[SIFIVE_U500_CLINT].base,
        memmap[SIFIVE_U500_CLINT].size, &s->soc,
        SIFIVE_SIP_BASE, SIFIVE_TIMECMP_BASE, SIFIVE_TIME_BASE);
}

static int riscv_sifive_u500_sysbus_device_init(SysBusDevice *sysbusdev)
{
    return 0;
}

static void riscv_sifive_u500_class_init(ObjectClass *klass, void *data)
{
    SysBusDeviceClass *k = SYS_BUS_DEVICE_CLASS(klass);
    k->init = riscv_sifive_u500_sysbus_device_init;
}

static const TypeInfo riscv_sifive_u500_device = {
    .name          = TYPE_SIFIVE_U500,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SiFiveU500State),
    .class_init    = riscv_sifive_u500_class_init,
};

static void riscv_sifive_u500_register_types(void)
{
    type_register_static(&riscv_sifive_u500_device);
}

type_init(riscv_sifive_u500_register_types);

static void riscv_sifive_u500_machine_init(MachineClass *mc)
{
    mc->desc = "RISC-V Board compatible with SiFive U500 SDK";
    mc->init = riscv_sifive_u500_init;
    mc->max_cpus = 1;
}

DEFINE_MACHINE("sifive_u500", riscv_sifive_u500_machine_init)
