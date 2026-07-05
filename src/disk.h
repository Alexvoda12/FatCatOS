// src/disk.h
#ifndef DISK_H
#define DISK_H

#include <stdbool.h>
#include <stdint.h>

#define SECTOR_SIZE 512

// Инициализация диска (проверка наличия)
bool disk_init();

// Чтение одного сектора LBA28
bool disk_read_sector(uint32_t lba, uint8_t *buffer);

// Запись одного сектора LBA28
bool disk_write_sector(uint32_t lba, const uint8_t *buffer);

#endif