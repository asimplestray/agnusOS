# Toolchain
CC = gcc
ASM = nasm
LD = ld

# Compiler Flags
# -m64: Generate 64-bit code
# -ffreestanding: No host standard library
# -mno-red-zone: Do not use x86_64 red zone (crucial for interrupt safety)
# -mno-mmx -mno-sse -mno-sse2: Disable vector units until kernel initializes them
# -fno-stack-protector -fno-builtin: Disable host-specific runtime supports
# -fno-pie -no-pie: Compile static, non-position-independent binary
CFLAGS = -m64 -ffreestanding -O2 -Wall -Wextra -Ikernel/include \
         -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
         -fno-stack-protector -fno-builtin -fno-pie

# Assembler Flags
ASMFLAGS = -f elf64

# Linker Flags
LDFLAGS = -n -nostdlib -T linker.ld -m elf_x86_64 --no-warn-rwx-segments

# Files
OBJ = build/boot.o build/interrupts.o build/idt.o build/gdt.o build/gdt_asm.o build/kernel.o \
      build/screen.o build/framebuffer.o build/timer.o build/keyboard.o build/tty.o \
      build/pmm.o build/vmm.o build/kheap.o build/pci.o \
      build/task.o build/task_switch.o build/syscall.o build/syscall_asm.o \
      build/vfs.o build/ramfs.o build/elf.o build/pipe.o \
      build/procfs.o build/devfs.o build/bcache.o \
      build/ata.o build/fat32.o build/firmware.o build/workqueue.o build/dma.o build/apollo_drv.o build/polaris.o build/serial.o \
      build/rtc.o build/panic.o build/drm_device.o \
      build/rtl8139.o build/net_core.o build/arp.o build/ip.o build/icmp.o build/udp.o build/loopback.o build/bsdsocket.o \
      build/drm_gem.o \
      build/assign.o build/msgport.o \
      build/exec.o build/exec_library.o build/exec_mempool.o \
      build/dos.o build/dos_path.o \
      build/string.o \
      build/dma_fence.o build/dma_resv.o build/dma_test.o \
      build/dma_buf.o build/drm_sched.o build/drm_atomic.o build/compat_check.o \
      build/amdgpu_device.o build/amdgpu_vram_mgr.o build/amdgpu_mode.o \
      build/amdgpu_gfx.o build/amdgpu_fw.o build/thermal_monitor.o build/gpu_test_pattern.o \
      build/dc_core.o build/dce_resource.o build/dcn_resource.o \
      build/dogin.o \
      build/acpi.o build/iommu.o

# Output
ISO_OUT = agnusos.iso
BIN_OUT = build/iso/boot/agnusos.bin
FW_CPIO = build/fw.cpio

.PHONY: all clean run

all: $(ISO_OUT)

$(FW_CPIO): scripts/make_fw_initrd.sh
	@mkdir -p build
	@echo ">> Packaging firmware CPIO..."
	@bash scripts/make_fw_initrd.sh

$(ISO_OUT): $(OBJ) linker.ld grub.cfg limine.cfg $(FW_CPIO) limine/BOOTX64.EFI limine/limine-bios.sys limine/limine-bios-cd.bin limine/limine-uefi-cd.bin
	@echo ">> Creating build directories..."
	@mkdir -p build/iso/boot
	@mkdir -p build/iso/EFI/BOOT
	@echo ">> Linking kernel..."
	@$(LD) $(LDFLAGS) -o $(BIN_OUT) $(OBJ)
	@echo ">> Verifying Multiboot2 header..."
	@grub-file --is-x86-multiboot2 $(BIN_OUT)
	@echo ">> Copying firmware archive..."
	@cp $(FW_CPIO) build/iso/boot/fw.cpio
	@echo ">> Generating Limine hybrid ISO (BIOS+UEFI)..."
	@cp limine.cfg build/iso/boot/limine.cfg
	@cp limine.cfg build/iso/limine.cfg
	@cp limine/limine-bios.sys build/iso/boot/limine-bios.sys
	@cp limine/limine-bios.sys build/iso/limine-bios.sys
	@cp limine/BOOTX64.EFI build/iso/EFI/BOOT/BOOTX64.EFI
	@cp limine/limine-bios-cd.bin build/iso/boot/limine-bios-cd.bin
	@cp limine/limine-uefi-cd.bin build/iso/boot/limine-uefi-cd.bin
	@xorriso -as mkisofs -b boot/limine-bios-cd.bin -no-emul-boot -boot-load-size 4 -boot-info-table --efi-boot boot/limine-uefi-cd.bin -efi-boot-part --efi-boot-image --protective-msdos-label build/iso -o $(ISO_OUT) 2>/dev/null
	@./limine/limine bios-install $(ISO_OUT) 2>/dev/null
	@echo ">> Success! Generated $(ISO_OUT) (Limine BIOS+UEFI, GRUB fallback: make grub-iso)"

grub-iso: $(OBJ) linker.ld grub.cfg $(FW_CPIO)
	@echo ">> [GRUB fallback] Creating build directories..."
	@mkdir -p build/iso/boot/grub
	@echo ">> Linking kernel..."
	@$(LD) $(LDFLAGS) -o $(BIN_OUT) $(OBJ)
	@echo ">> Verifying Multiboot2 header..."
	@grub-file --is-x86-multiboot2 $(BIN_OUT)
	@echo ">> Copying firmware archive..."
	@cp $(FW_CPIO) build/iso/boot/fw.cpio
	@echo ">> Generating GRUB ISO..."
	@cp grub.cfg build/iso/boot/grub/grub.cfg
	@grub-mkrescue -o $(ISO_OUT) build/iso
	@echo ">> Success! Generated $(ISO_OUT) (GRUB)"

build/boot.o: boot/boot.asm
	@mkdir -p build
	@echo ">> Assembling $<..."
	@$(ASM) $(ASMFLAGS) -o $@ $<

build/interrupts.o: kernel/cpu/interrupts.asm
	@mkdir -p build
	@echo ">> Assembling $<..."
	@$(ASM) $(ASMFLAGS) -o $@ $<

build/idt.o: kernel/cpu/idt.c
	@mkdir -p build
	@echo ">> Compiling $<..."
	@$(CC) $(CFLAGS) -c -o $@ $<

build/kernel.o: kernel/kernel.c
	@mkdir -p build
	@echo ">> Compiling $<..."
	@$(CC) $(CFLAGS) -c -o $@ $<

build/screen.o: kernel/drivers/screen.c
	@mkdir -p build
	@echo ">> Compiling $<..."
	@$(CC) $(CFLAGS) -c -o $@ $<

build/framebuffer.o: kernel/drivers/framebuffer.c
	@mkdir -p build
	@echo ">> Compiling $<..."
	@$(CC) $(CFLAGS) -c -o $@ $<

build/timer.o: kernel/drivers/timer.c
	@mkdir -p build
	@echo ">> Compiling $<..."
	@$(CC) $(CFLAGS) -c -o $@ $<

build/keyboard.o: kernel/drivers/keyboard.c
	@mkdir -p build
	@echo ">> Compiling $<..."
	@$(CC) $(CFLAGS) -c -o $@ $<

build/tty.o: kernel/drivers/tty.c
	@mkdir -p build
	@echo ">> Compiling $<..."
	@$(CC) $(CFLAGS) -c -o $@ $<

build/pmm.o: kernel/mem/pmm.c
	@mkdir -p build
	@echo ">> Compiling $<..."
	@$(CC) $(CFLAGS) -c -o $@ $<

build/vmm.o: kernel/mem/vmm.c
	@mkdir -p build
	@echo ">> Compiling $<..."
	@$(CC) $(CFLAGS) -c -o $@ $<

build/kheap.o: kernel/mem/kheap.c
	@mkdir -p build
	@echo ">> Compiling $<..."
	@$(CC) $(CFLAGS) -c -o $@ $<

build/task.o: kernel/task.c
	@mkdir -p build
	@echo ">> Compiling $<..."
	@$(CC) $(CFLAGS) -c -o $@ $<

build/task_switch.o: kernel/task_switch.asm
	@mkdir -p build
	@echo ">> Assembling $<..."
	@$(ASM) $(ASMFLAGS) -o $@ $<

build/syscall.o: kernel/syscall.c
	@mkdir -p build
	@echo ">> Compiling $<..."
	@$(CC) $(CFLAGS) -c -o $@ $<

build/syscall_asm.o: kernel/cpu/syscall.asm
	@mkdir -p build
	@echo ">> Assembling $<..."
	@$(ASM) $(ASMFLAGS) -o $@ $<

build/pci.o: kernel/pci/pci.c
	@mkdir -p build
	@echo ">> Compiling $<..."
	@$(CC) $(CFLAGS) -c -o $@ $<

build/apollo_drv.o: kernel/drivers/gpu/apollo/apollo_drv.c
	@mkdir -p build
	@echo ">> Compiling $<..."
	@$(CC) $(CFLAGS) -c -o $@ $<

build/polaris.o: kernel/drivers/gpu/apollo/polaris.c
	@mkdir -p build
	@echo ">> Compiling $<..."
	@$(CC) $(CFLAGS) -c -o $@ $<

build/serial.o: kernel/drivers/serial.c
	@mkdir -p build
	@echo ">> Compiling $<..."
	@$(CC) $(CFLAGS) -c -o $@ $<

build/rtc.o: kernel/drivers/rtc.c
	@mkdir -p build
	@echo ">> Compiling $<..."
	@$(CC) $(CFLAGS) -c -o $@ $<

build/panic.o: kernel/kernel/panic.c
	@mkdir -p build
	@echo ">> Compiling $<..."
	@$(CC) $(CFLAGS) -c -o $@ $<

build/drm_device.o: kernel/drivers/drm/drm_device.c
	@mkdir -p build
	@echo ">> Compiling $<..."
	@$(CC) $(CFLAGS) -c -o $@ $<

build/rtl8139.o: kernel/drivers/net/rtl8139.c
	@mkdir -p build
	@echo ">> Compiling $<..."
	@$(CC) $(CFLAGS) -c -o $@ $<

build/net_core.o: kernel/net/core.c
	@mkdir -p build
	@echo ">> Compiling $<..."
	@$(CC) $(CFLAGS) -c -o $@ $<

build/arp.o: kernel/net/arp.c
	@mkdir -p build
	@echo ">> Compiling $<..."
	@$(CC) $(CFLAGS) -c -o $@ $<

build/ip.o: kernel/net/ip.c
	@mkdir -p build
	@echo ">> Compiling $<..."
	@$(CC) $(CFLAGS) -c -o $@ $<

build/icmp.o: kernel/net/icmp.c
	@mkdir -p build
	@echo ">> Compiling $<..."
	@$(CC) $(CFLAGS) -c -o $@ $<

build/udp.o: kernel/net/udp.c
	@mkdir -p build
	@echo ">> Compiling $<..."
	@$(CC) $(CFLAGS) -c -o $@ $<

build/loopback.o: kernel/net/loopback.c
	@mkdir -p build
	@echo ">> Compiling $<..."
	@$(CC) $(CFLAGS) -c -o $@ $<

build/bsdsocket.o: kernel/net/bsdsocket.c
	@mkdir -p build
	@echo ">> Compiling $<..."
	@$(CC) $(CFLAGS) -c -o $@ $<

build/string.o: kernel/lib/string.c
	@mkdir -p build
	@echo ">> Compiling $<..."
	@$(CC) $(CFLAGS) -c -o $@ $<

build/gdt.o: kernel/cpu/gdt.c
	@mkdir -p build
	@echo ">> Compiling $<..."
	@$(CC) $(CFLAGS) -c -o $@ $<

build/gdt_asm.o: kernel/cpu/gdt.asm
	@mkdir -p build
	@echo ">> Assembling $<..."
	@$(ASM) $(ASMFLAGS) -o $@ $<

build/vfs.o: kernel/fs/vfs.c
	@mkdir -p build
	@echo ">> Compiling $<..."
	@$(CC) $(CFLAGS) -c -o $@ $<

build/ramfs.o: kernel/fs/ramfs.c
	@mkdir -p build
	@echo ">> Compiling $<..."
	@$(CC) $(CFLAGS) -c -o $@ $<

build/pipe.o: kernel/fs/pipe.c
	@mkdir -p build
	@echo ">> Compiling $<..."
	@$(CC) $(CFLAGS) -c -o $@ $<

build/procfs.o: kernel/fs/procfs.c
	@mkdir -p build
	@echo ">> Compiling $<..."
	@$(CC) $(CFLAGS) -c -o $@ $<

build/devfs.o: kernel/fs/devfs.c
	@mkdir -p build
	@echo ">> Compiling $<..."
	@$(CC) $(CFLAGS) -c -o $@ $<

build/bcache.o: kernel/fs/bcache.c
	@mkdir -p build
	@echo ">> Compiling $<..."
	@$(CC) $(CFLAGS) -c -o $@ $<

build/elf.o: kernel/loader/elf.c
	@mkdir -p build
	@echo ">> Compiling $<..."
	@$(CC) $(CFLAGS) -c -o $@ $<

build/ata.o: kernel/drivers/ata.c
	@mkdir -p build
	@echo ">> Compiling $<..."
	@$(CC) $(CFLAGS) -c -o $@ $<

build/fat32.o: kernel/fs/fat32.c
	@mkdir -p build
	@echo ">> Compiling $<..."
	@$(CC) $(CFLAGS) -c -o $@ $<

build/firmware.o: kernel/fs/firmware.c
	@mkdir -p build
	@echo ">> Compiling $<..."
	@$(CC) $(CFLAGS) -c -o $@ $<

build/workqueue.o: kernel/kernel/workqueue.c
	@mkdir -p build
	@echo ">> Compiling $<..."
	@$(CC) $(CFLAGS) -c -o $@ $<

build/dma.o: kernel/kernel/dma.c
	@mkdir -p build
	@echo ">> Compiling $<..."
	@$(CC) $(CFLAGS) -c -o $@ $<

build/dma_fence.o: kernel/drivers/drm/dma_fence.c
	@mkdir -p build
	@echo ">> Compiling $<..."
	@$(CC) $(CFLAGS) -c -o $@ $<

build/dma_resv.o: kernel/drivers/drm/dma_resv.c
	@mkdir -p build
	@echo ">> Compiling $<..."
	@$(CC) $(CFLAGS) -c -o $@ $<

build/dma_test.o: kernel/drivers/drm/dma_test.c
	@mkdir -p build
	@echo ">> Compiling $<..."
	@$(CC) $(CFLAGS) -c -o $@ $<

build/drm_gem.o: kernel/drivers/drm/drm_gem.c kernel/include/drm/drm_gem.h
	@mkdir -p build
	@echo ">> Compiling $<..."
	@$(CC) $(CFLAGS) -c -o $@ $<

build/dma_buf.o: kernel/drivers/drm/dma_buf.c kernel/include/drm/dma_buf.h
	@mkdir -p build
	@echo ">> Compiling $<..."
	@$(CC) $(CFLAGS) -c -o $@ $<

build/drm_sched.o: kernel/drivers/drm/drm_sched.c kernel/include/drm/drm_sched.h
	@mkdir -p build
	@echo ">> Compiling $<..."
	@$(CC) $(CFLAGS) -c -o $@ $<

build/drm_atomic.o: kernel/drivers/drm/drm_atomic.c kernel/include/drm/drm_atomic.h
	@mkdir -p build
	@echo ">> Compiling $<..."
	@$(CC) $(CFLAGS) -c -o $@ $<

build/compat_check.o: kernel/drivers/drm/compat_check.c
	@mkdir -p build
	@echo ">> Compiling $<..."
	@$(CC) $(CFLAGS) -c -o $@ $<

build/amdgpu_device.o: kernel/drivers/gpu/amd/amdgpu/amdgpu_device.c kernel/include/amdgpu.h
	@mkdir -p build
	@echo ">> Compiling $<..."
	@$(CC) $(CFLAGS) -c -o $@ $<

build/amdgpu_vram_mgr.o: kernel/drivers/gpu/amd/amdgpu/amdgpu_vram_mgr.c kernel/include/amdgpu.h
	@mkdir -p build
	@echo ">> Compiling $<..."
	@$(CC) $(CFLAGS) -c -o $@ $<

build/amdgpu_mode.o: kernel/drivers/gpu/amd/amdgpu/amdgpu_mode.c kernel/include/amdgpu.h
	@mkdir -p build
	@echo ">> Compiling $<..."
	@$(CC) $(CFLAGS) -c -o $@ $<

build/amdgpu_gfx.o: kernel/drivers/gpu/amd/amdgpu/amdgpu_gfx.c kernel/include/amdgpu.h
	@mkdir -p build
	@echo ">> Compiling $<..."
	@$(CC) $(CFLAGS) -c -o $@ $<

build/amdgpu_fw.o: kernel/drivers/gpu/amd/amdgpu/amdgpu_fw.c kernel/include/amdgpu.h kernel/include/firmware.h
	@mkdir -p build
	@echo ">> Compiling $<..."
	@$(CC) $(CFLAGS) -c -o $@ $<

build/thermal_monitor.o: kernel/drivers/gpu/amd/amdgpu/thermal_monitor.c kernel/include/amdgpu.h
	@mkdir -p build
	@echo ">> Compiling $<..."
	@$(CC) $(CFLAGS) -c -o $@ $<

build/gpu_test_pattern.o: kernel/drivers/gpu/amd/amdgpu/gpu_test_pattern.c kernel/include/amdgpu.h kernel/include/amdgpu_dc.h
	@mkdir -p build
	@echo ">> Compiling $<..."
	@$(CC) $(CFLAGS) -c -o $@ $<

build/dc_core.o: kernel/drivers/gpu/amd/amdgpu/dc/dc_core.c kernel/include/amdgpu_dc.h
	@mkdir -p build
	@echo ">> Compiling $<..."
	@$(CC) $(CFLAGS) -c -o $@ $<

build/dce_resource.o: kernel/drivers/gpu/amd/amdgpu/dc/dce_resource.c kernel/include/amdgpu_dc.h
	@mkdir -p build
	@echo ">> Compiling $<..."
	@$(CC) $(CFLAGS) -c -o $@ $<

build/dcn_resource.o: kernel/drivers/gpu/amd/amdgpu/dc/dcn_resource.c kernel/include/amdgpu_dc.h
	@mkdir -p build
	@echo ">> Compiling $<..."
	@$(CC) $(CFLAGS) -c -o $@ $<

build/assign.o: kernel/fs/assign.c kernel/include/assign.h
	@mkdir -p build
	@echo ">> Compiling $<..."
	@$(CC) $(CFLAGS) -c -o $@ $<

build/msgport.o: kernel/ipc/msgport.c kernel/include/msgport.h
	@mkdir -p build
	@echo ">> Compiling $<..."
	@$(CC) $(CFLAGS) -c -o $@ $<

build/exec.o: kernel/exec/exec.c kernel/include/exec/exec.h kernel/include/exec/task.h kernel/include/exec/library.h kernel/include/exec/types.h
	@mkdir -p build
	@echo ">> Compiling $<..."
	@$(CC) $(CFLAGS) -c -o $@ $<

build/exec_mempool.o: kernel/exec/mempool.c kernel/include/exec/exec.h kernel/include/exec/task.h kernel/include/exec/types.h
	@mkdir -p build
	@echo ">> Compiling $<..."
	@$(CC) $(CFLAGS) -c -o $@ $<

build/exec_library.o: kernel/exec/library.c kernel/include/exec/library.h kernel/include/exec/exec.h
	@mkdir -p build
	@echo ">> Compiling $<..."
	@$(CC) $(CFLAGS) -c -o $@ $<

build/dos.o: kernel/dos/dos.c kernel/include/dos/dos.h kernel/include/dos/types.h kernel/include/vfs.h kernel/include/assign.h
	@mkdir -p build
	@echo ">> Compiling $<..."
	@$(CC) $(CFLAGS) -c -o $@ $<

build/dos_path.o: kernel/dos/path.c kernel/include/dos/path.h
	@mkdir -p build
	@echo ">> Compiling $<..."
	@$(CC) $(CFLAGS) -c -o $@ $<

build/dogin.o: kernel/dogin/dogin.c kernel/include/dogin.h
	@mkdir -p build
	@echo ">> Compiling $<..."
	@$(CC) $(CFLAGS) -c -o $@ $<

build/acpi.o: kernel/acpi/acpi.c kernel/include/acpi.h kernel/include/multiboot2.h
	@mkdir -p build
	@echo ">> Compiling $<..."
	@$(CC) $(CFLAGS) -c -o $@ $<

build/iommu.o: kernel/drivers/iommu.c kernel/include/iommu.h kernel/include/acpi.h
	@mkdir -p build
	@echo ">> Compiling $<..."
	@$(CC) $(CFLAGS) -c -o $@ $<

clean:
	@echo ">> Cleaning build artifacts..."
	@rm -rf build $(ISO_OUT)

run: all
	@echo ">> Running AgnusOS in QEMU (requires qemu-desktop for GUI)..."
	@test -f disk.img || qemu-img create -q -f raw disk.img 64M
	qemu-system-x86_64 -m 512M -cdrom $(ISO_OUT) -drive if=none,format=raw,id=disk0,file=disk.img -device virtio-blk-pci,drive=disk0 -vga std -display sdl

# Fallback: VNC display (connect with: vncviewer localhost:5900)
run-vnc: all
	@echo ">> Running AgnusOS via VNC on localhost:5900 ..."
	@echo "   Connect with: vncviewer localhost:5900"
	@test -f disk.img || qemu-img create -q -f raw disk.img 64M
	qemu-system-x86_64 -m 512M -cdrom $(ISO_OUT) -drive if=none,format=raw,id=disk0,file=disk.img -device virtio-blk-pci,drive=disk0 -vga std -vnc :0

