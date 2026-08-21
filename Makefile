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
      build/screen.o build/timer.o build/keyboard.o build/tty.o \
      build/pmm.o build/vmm.o build/kheap.o build/pci.o \
      build/task.o build/syscall.o build/syscall_asm.o \
      build/vfs.o build/ramfs.o build/elf.o build/pipe.o \
      build/procfs.o build/devfs.o build/bcache.o \
      build/ata.o build/fat32.o build/firmware.o build/workqueue.o build/dma.o build/apollo_drv.o build/polaris.o build/serial.o \
      build/rtc.o build/panic.o build/drm_device.o \
      build/rtl8139.o build/net_core.o build/arp.o build/ip.o build/icmp.o build/udp.o build/loopback.o \
      build/drm_gem.o \
      build/string.o \
      build/dma_fence.o build/dma_resv.o build/dma_test.o \
      build/dma_buf.o build/drm_sched.o build/drm_atomic.o build/compat_check.o

# Output
ISO_OUT = apolloos.iso
BIN_OUT = build/iso/boot/apolloos.bin

.PHONY: all clean run

all: $(ISO_OUT)

$(ISO_OUT): $(OBJ) linker.ld grub.cfg
	@echo ">> Creating build directories..."
	@mkdir -p build/iso/boot/grub
	@echo ">> Linking kernel..."
	@$(LD) $(LDFLAGS) -o $(BIN_OUT) $(OBJ)
	@echo ">> Verifying Multiboot2 header..."
	@grub-file --is-x86-multiboot2 $(BIN_OUT)
	@echo ">> Generating ISO..."
	@cp grub.cfg build/iso/boot/grub/grub.cfg
	@grub-mkrescue -o $(ISO_OUT) build/iso
	@echo ">> Success! Generated $(ISO_OUT)"

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

clean:
	@echo ">> Cleaning build artifacts..."
	@rm -rf build $(ISO_OUT)

run: all
	@echo ">> Running ApolloOS in QEMU (requires qemu-desktop for GUI)..."
	qemu-system-x86_64 -m 512M -cdrom $(ISO_OUT) -hda disk.img -vga std -display sdl

# Fallback: VNC display (connect with: vncviewer localhost:5900)
run-vnc: all
	@echo ">> Running ApolloOS via VNC on localhost:5900 ..."
	@echo "   Connect with: vncviewer localhost:5900"
	qemu-system-x86_64 -m 512M -cdrom $(ISO_OUT) -hda disk.img -vga std -vnc :0

