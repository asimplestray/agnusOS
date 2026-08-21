#include <screen.h>
#include <multiboot2.h>
#include <idt.h>
#include <gdt.h>
#include <timer.h>
#include <keyboard.h>
#include <pmm.h>
#include <vmm.h>
#include <kheap.h>
#include <pci.h>
#include <task.h>
#include <vfs.h>
#include <syscall.h>
#include <fat32.h>
#include <procfs.h>
#include <devfs.h>
#include <firmware.h>
#include <apollo_drv.h>
#include <serial.h>
#include <polaris.h>
#include <drm/drm_gem.h>
#include <rtc.h>
#include <panic.h>
#include <tty.h>
#include <net/net.h>
#include <rtl8139.h>
#include <drm/drm_driver.h>
#include <drm/dma_test.h>
extern uint32_t multiboot_magic;
extern uint64_t multiboot_info;

void kernel_main(void) {
    serial_init();
    log_init();
    serial_print("ApolloOS: Starting kernel...\n");
    
    screen_init();
    screen_set_color(COLOR_LIGHT_GREY, COLOR_BLACK);
    screen_print("===== ApolloOS v0.2-Alpha (Texto) =====\n");
    screen_print("Kernel x86_64 - Modo VGA texto simples\n\n");
    serial_print("ApolloOS: screen_init done\n");

    idt_init();
    serial_print("ApolloOS: idt_init done\n");
    screen_log("OK", COLOR_LIGHT_GREEN, "IDT carregada & PIC remapeado.");

    gdt_init();
    serial_print("ApolloOS: gdt_init done\n");
    screen_log("OK", COLOR_LIGHT_GREEN, "GDT/TSS propria carregada.");

    screen_log("OK", COLOR_LIGHT_GREEN, "CPU 64-bit Long Mode ativa.");
    serial_print("ApolloOS: Long mode confirmed\n");

    if (multiboot_magic == MULTIBOOT2_MAGIC) {
        pmm_init(multiboot_info);
        serial_print("ApolloOS: pmm_init done\n");
        screen_log("OK", COLOR_LIGHT_GREEN, "PMM inicializado.");

        screen_set_color(COLOR_LIGHT_CYAN, COLOR_BLACK);
        screen_print("RAM Total: ");
        // simplificado - sem função de print dec
        screen_set_color(COLOR_WHITE, COLOR_BLACK);

        vmm_init();
        serial_print("ApolloOS: vmm_init done\n");
        screen_log("OK", COLOR_LIGHT_GREEN, "VMM inicializado.");

        kheap_init();
        serial_print("ApolloOS: kheap_init done\n");
        screen_log("OK", COLOR_LIGHT_GREEN, "Kernel heap ok.");

        vfs_init();
        serial_print("ApolloOS: vfs_init done\n");
        screen_log("OK", COLOR_LIGHT_GREEN, "VFS inicializado (RamFS montado em /).");

        procfs_init();
        serial_print("ApolloOS: procfs_init done\n");

        devfs_init();
        serial_print("ApolloOS: devfs_init done\n");

        drm_init();
        serial_print("ApolloOS: drm_init done\n");

        /* Parse multiboot2 for initrd module */
        void *initrd_addr = NULL;
        size_t initrd_size = 0;
        if (multiboot_magic == MULTIBOOT2_MAGIC) {
            struct multiboot_tag *tag = (struct multiboot_tag *)(multiboot_info + 8);
            while (tag->type != MULTIBOOT_TAG_TYPE_END) {
                if (tag->type == MULTIBOOT_TAG_TYPE_MODULE) {
                    struct multiboot_tag_module *mod = (struct multiboot_tag_module *)tag;
                    initrd_addr = (void *)(uintptr_t)mod->mod_start;
                    initrd_size = mod->mod_end - mod->mod_start;
                    break;
                }
                tag = (struct multiboot_tag *)(((uintptr_t)tag) + ((tag->size + 7) & ~7));
            }
        }
        
        /* Initialize firmware cache from initrd */
        firmware_cache_init(initrd_addr, initrd_size);
        serial_print("ApolloOS: firmware_cache_init done\n");

        /* Mount FAT32 partition on /fat32 */
        vfs_node_t *fat32_node = vfs_resolve("/fat32");
        if (fat32_node) {
            fat32_init_and_mount(fat32_node);
            serial_print("ApolloOS: fat32_init_and_mount done\n");
        }
        
        task_init();
        serial_print("ApolloOS: task_init done\n");
        screen_log("OK", COLOR_LIGHT_GREEN, "Task scheduler ok.");
        
        pci_enum();
        serial_print("ApolloOS: pci_enum done\n");
        screen_log("OK", COLOR_LIGHT_GREEN, "PCI enumeration completa.");
        
        /* Fase 1 Dev 2: GEM selftest (dev3_test-style probe) */
        drm_gem_test();

        // Try to initialize Apollo GPU driver (RX 580 / Polaris)
        for (int bus = 0; bus < 256; bus++) {
            for (int dev = 0; dev < 32; dev++) {
                uint16_t vendor = pci_read_word((uint8_t)bus, (uint8_t)dev, 0, 0);
                if (vendor == 0x1002) {
                    uint16_t dev_id = pci_read_word((uint8_t)bus, (uint8_t)dev, 0, 2);
                    // RX 580 (Polaris 20) / RX 480 (Polaris 10)
                    if (dev_id == 0x67DF || dev_id == 0x67EF || dev_id == 0x67FF) {
                        serial_print("ApolloOS: Found Polaris GPU, initializing driver\n");
                        struct polaris_dev pdev;
                        if (polaris_init(&pdev, (uint8_t)bus, (uint8_t)dev, 0) == 0) {
                            polaris_set_mode(&pdev, 1920, 1080, 32);
                            polaris_test_pattern(&pdev);
                            screen_log("OK", COLOR_LIGHT_GREEN, "Polaris hardware initialized (1920x1080x32).");
                            serial_print("ApolloOS: ApolloGPU driver initialized\n");
                        }
                    }
                }
            }
        }
    } else {
        screen_log("FALHA", COLOR_LIGHT_RED, "Multiboot2 invalido.");
    }

    timer_init(100);
    serial_print("ApolloOS: timer_init done\n");
    screen_log("OK", COLOR_LIGHT_GREEN, "Timer PIT 100Hz.");

    rtc_init();
    serial_print("ApolloOS: rtc_init done\n");
    screen_log("OK", COLOR_LIGHT_GREEN, "RTC/CMOS initialized.");

    keyboard_init();
    keyboard_set_layout(LAYOUT_ABNT2);
    serial_print("ApolloOS: keyboard_init done\n");
    screen_log("OK", COLOR_LIGHT_GREEN, "Teclado PS/2 (ABNT2).");

    tty_init();
    serial_print("ApolloOS: tty_init done\n");
    screen_log("OK", COLOR_LIGHT_GREEN, "TTY line discipline initialized.");
    
    syscall_init();
    serial_print("ApolloOS: syscall_init done\n");
    screen_log("OK", COLOR_LIGHT_GREEN, "Tabela de syscalls inicializada.");

    net_init();
    serial_print("ApolloOS: net_init done\n");
    screen_log("OK", COLOR_LIGHT_GREEN, "Network stack initialized.");

    loopback_init();
    serial_print("ApolloOS: loopback_init done\n");
    screen_log("OK", COLOR_LIGHT_GREEN, "Loopback interface (127.0.0.1) up.");

    for (int bus = 0; bus < 256; bus++) {
        for (int dev = 0; dev < 32; dev++) {
            uint16_t vendor = pci_read_word((uint8_t)bus, (uint8_t)dev, 0, 0);
            if (vendor == RTL8139_VENDOR_ID) {
                uint16_t dev_id = pci_read_word((uint8_t)bus, (uint8_t)dev, 0, 2);
                if (dev_id == RTL8139_DEVICE_ID) {
                    static struct rtl8139_dev rtl_dev;
                    if (rtl8139_init(&rtl_dev, bus, dev, 0) == 0) {
                        struct netif *netif = netif_alloc("eth0");
                        if (netif) {
                            rtl8139_get_mac(&rtl_dev, netif->mac);
                            netif->ip[0] = 10; netif->ip[1] = 0; netif->ip[2] = 2; netif->ip[3] = 15;
                            netif->netmask[0] = 255; netif->netmask[1] = 255; netif->netmask[2] = 255; netif->netmask[3] = 0;
                            netif->xmit = rtl8139_xmit;
                            rtl8139_set_rx_handler(&rtl_dev, netif);
                            netif_register(netif);
                            netif_default = netif;
                            screen_log("OK", COLOR_LIGHT_GREEN, "RTL8139 ethernet initialized");
                        }
                    }
                }
            }
        }
    }
    serial_print("ApolloOS: syscall_init done\n");
    screen_log("OK", COLOR_LIGHT_GREEN, "Tabela de syscalls inicializada.");

    __asm__ volatile("sti");
    serial_print("ApolloOS: STI - interrupts enabled\n");
    screen_log("OK", COLOR_LIGHT_GREEN, "Interrupcoes habilitadas (STI).");
    
    /* Run DMA fence/resv tests */
    dma_test_run_all();

    screen_print("\n>> ApolloOS pronto. Iniciando processo usuario...\n");

    /* Launch /bin/hello from RamFS in Ring 3 */
    /* task_create_user("/bin/hello"); */

    /* task_create_user() calls jump_to_usermode which does not return.
     * If we get here somehow, spin safely. */
    while (1) {
        __asm__ volatile("hlt");
    }
}
