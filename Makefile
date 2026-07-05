CC = gcc
ASM = nasm
LD = ld

CFLAGS = -m32 -ffreestanding -nostdlib -fno-pie -Wall -Wextra -O0 -g
ASMFLAGS = -f elf32
LDFLAGS = -T src/linker.ld -m elf_i386 -nostdlib

BUILD_DIR = build
ISO_DIR = $(BUILD_DIR)/iso
DISK_IMG = $(BUILD_DIR)/disk.img

.PHONY: all clean iso run

all: $(BUILD_DIR)/kernel.bin iso

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)/boot/grub

$(BUILD_DIR)/boot.o: src/boot.asm | $(BUILD_DIR)
	$(ASM) $(ASMFLAGS) -o $@ $<

$(BUILD_DIR)/kernel.o: src/kernel.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/disk.o: src/disk.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/shell.o: src/shell.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/kernel.bin: $(BUILD_DIR)/boot.o $(BUILD_DIR)/kernel.o $(BUILD_DIR)/disk.o $(BUILD_DIR)/shell.o
	$(LD) $(LDFLAGS) -o $@ $^
	grub-file --is-x86-multiboot $@

iso: $(BUILD_DIR)/kernel.bin $(DISK_IMG)
	mkdir -p $(ISO_DIR)/boot/grub
	cp $< $(ISO_DIR)/boot/kernel.bin
	cp grub.cfg $(ISO_DIR)/boot/grub/grub.cfg
	cp $(DISK_IMG) $(ISO_DIR)/disk.img
	grub-mkrescue -o $(BUILD_DIR)/fatcat_os.iso $(ISO_DIR)

$(DISK_IMG):
	dd if=/dev/zero of=$@ bs=1M count=10   # 10 МБ диск

run: all
	qemu-system-i386 -cdrom $(BUILD_DIR)/fatcat_os.iso -drive file=$(DISK_IMG),format=raw,if=ide

clean:
	rm -rf $(BUILD_DIR)