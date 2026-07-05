// src/disk.c
#include "disk.h"

// Порты первичного IDE-контроллера (Primary Master)
#define DATA_PORT      0x1F0
#define ERROR_PORT     0x1F1
#define SECTOR_COUNT   0x1F2
#define LBA_LO         0x1F3
#define LBA_MID        0x1F4
#define LBA_HI         0x1F5
#define DRIVE_SELECT   0x1F6
#define COMMAND_PORT   0x1F7
#define STATUS_PORT    0x1F7

// Статусные флаги
#define STATUS_BSY     0x80
#define STATUS_RDY     0x40
#define STATUS_DRQ     0x08
#define STATUS_ERR     0x01

// Команды ATA
#define CMD_READ       0x20   // read sectors with retry
#define CMD_WRITE      0x30   // write sectors with retry

// Чтение/запись в порты
static inline uint8_t inb(uint16_t port) {
    uint8_t result;
    __asm__ volatile ("inb %1, %0" : "=a"(result) : "Nd"(port));
    return result;
}

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" :: "a"(val), "Nd"(port));
}

static inline void insw(uint16_t port, void *buf, int count) {
    __asm__ volatile ("rep insw" : "+D"(buf), "+c"(count) : "d"(port) : "memory");
}

static inline void outsw(uint16_t port, const void *buf, int count) {
    __asm__ volatile ("rep outsw" :: "S"(buf), "c"(count), "d"(port) : "memory");
}

// Ожидание готовности диска
static bool wait_drive_ready() {
    uint8_t status;
    for (int i = 0; i < 10000; i++) {
        status = inb(STATUS_PORT);
        if (!(status & STATUS_BSY) && (status & STATUS_RDY))
            return true;
        // небольшая задержка
        for (volatile int j = 0; j < 1000; j++);
    }
    return false;
}

// Инициализация: убедимся, что диск готов
bool disk_init() {
    return wait_drive_ready();
}

// Чтение сектора
bool disk_read_sector(uint32_t lba, uint8_t *buffer) {
    // Ждём готовности
    if (!wait_drive_ready()) return false;

    // Выбираем master-диск, режим LBA28
    outb(DRIVE_SELECT, 0xE0 | ((lba >> 24) & 0x0F));
    outb(SECTOR_COUNT, 1);
    outb(LBA_LO, lba & 0xFF);
    outb(LBA_MID, (lba >> 8) & 0xFF);
    outb(LBA_HI, (lba >> 16) & 0xFF);
    outb(COMMAND_PORT, CMD_READ);

    // Ждём готовности данных
    uint8_t status = inb(STATUS_PORT);
    while (status & STATUS_BSY) status = inb(STATUS_PORT);
    if (status & STATUS_ERR) return false;
    if (!(status & STATUS_DRQ)) return false;

    // Читаем 256 слов (512 байт)
    insw(DATA_PORT, buffer, 256);
    return true;
}

// Запись сектора
bool disk_write_sector(uint32_t lba, const uint8_t *buffer) {
    if (!wait_drive_ready()) return false;

    outb(DRIVE_SELECT, 0xE0 | ((lba >> 24) & 0x0F));
    outb(SECTOR_COUNT, 1);
    outb(LBA_LO, lba & 0xFF);
    outb(LBA_MID, (lba >> 8) & 0xFF);
    outb(LBA_HI, (lba >> 16) & 0xFF);
    outb(COMMAND_PORT, CMD_WRITE);

    // Ждём готовности к приёму данных
    uint8_t status = inb(STATUS_PORT);
    while (status & STATUS_BSY) status = inb(STATUS_PORT);
    if (status & STATUS_ERR) return false;
    if (!(status & STATUS_DRQ)) return false;

    // Отправляем 256 слов
    outsw(DATA_PORT, buffer, 256);

    // Дожидаемся завершения записи (BUSY спадёт)
    while ((inb(STATUS_PORT) & STATUS_BSY));
    return true;
}