// src/shell.c - CMD-оболочка FatCatOS с файловой системой
#include <stdbool.h>
#include "disk.h"

// Внешние функции терминала (из kernel.c)
extern void putchar(char c);
extern void clear_screen();
extern void print_string(const char* str);
extern char get_scancode();
extern void scroll_up(void);
extern void scroll_down(void);
extern void select_start(void);
extern void select_move(int dx, int dy);
extern void select_clear(void);
extern void copy_selection(void);
extern void paste_clipboard(void);
extern void select_all(void);
extern bool shift_pressed;
extern bool ctrl_pressed;
extern void set_color(unsigned char fg, unsigned char bg);

// Скан-коды в ASCII
static const char scancode_to_ascii[] = {
    0, 0, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', 0,
    0, 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0,
    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\',
    'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, 0, 0, ' '
};

// Строковые функции
int strlen(const char* s) { int i = 0; while(s[i]) i++; return i; }
int strcmp(const char* a, const char* b) { while(*a && *a==*b){a++;b++;} return *a-*b; }
void strcpy(char* d, const char* s) { while(*s) *d++=*s++; *d=0; }
void strcat(char* d, const char* s) { while(*d)d++; while(*s)*d++=*s++; *d=0; }

// Параметры диска
#define SECTOR_SIZE 512
#define SUPERBLOCK_SECTOR 0
#define DIR_SECTOR_START  1
#define DIR_ENTRIES       32
#define DATA_SECTOR_START (DIR_SECTOR_START + DIR_SECTOR_COUNT)  // 1 + 2 = 3
#define MAX_FILES         32
#define MAX_FILENAME      12
#define DIR_ENTRY_SIZE    20   // 12 bytes name + 4 size + 4 start_sector
#define DIR_SECTOR_COUNT  2    // 2 сектора под каталог (до 51 записи, хватит 32)

// Структура записи каталога (в памяти)
typedef struct {
    char name[MAX_FILENAME];
    uint32_t size;
    uint32_t start_sector;
    bool used;
} dir_entry_t;

static dir_entry_t directory[MAX_FILES];
static bool fs_mounted = false;

// Чтение/запись суперблока
static bool read_superblock() {
    uint8_t buf[SECTOR_SIZE];
    if (!disk_read_sector(SUPERBLOCK_SECTOR, buf)) return false;
    if (buf[0]!='M' || buf[1]!='i' || buf[2]!='O' || buf[3]!='S' ||
        buf[4]!='F' || buf[5]!='S')
        return false;
    return true;
}

static bool write_superblock() {
    uint8_t buf[SECTOR_SIZE] = {0};
    buf[0]='M'; buf[1]='i'; buf[2]='O'; buf[3]='S'; buf[4]='F'; buf[5]='S';
    return disk_write_sector(SUPERBLOCK_SECTOR, buf);
}

// Запись каталога на диск
static bool write_directory() {
    uint8_t buf[DIR_SECTOR_COUNT * SECTOR_SIZE];
    for (int i = 0; i < DIR_SECTOR_COUNT * SECTOR_SIZE; i++) buf[i] = 0;
    for (int i = 0; i < MAX_FILES; i++) {
        uint8_t *entry = &buf[i * DIR_ENTRY_SIZE];
        int len = strlen(directory[i].name);
        for (int j = 0; j < MAX_FILENAME; j++) {
            entry[j] = (j < len) ? directory[i].name[j] : 0;
        }
        *(uint32_t*)&entry[12] = directory[i].size;
        *(uint32_t*)&entry[16] = directory[i].start_sector;
    }
    for (int s = 0; s < DIR_SECTOR_COUNT; s++) {
        if (!disk_write_sector(DIR_SECTOR_START + s, &buf[s * SECTOR_SIZE]))
            return false;
    }
    return true;
}

// Чтение каталога с диска
static bool read_directory() {
    uint8_t buf[DIR_SECTOR_COUNT * SECTOR_SIZE];
    for (int s = 0; s < DIR_SECTOR_COUNT; s++) {
        if (!disk_read_sector(DIR_SECTOR_START + s, &buf[s * SECTOR_SIZE]))
            return false;
    }
    for (int i = 0; i < MAX_FILES; i++) {
        uint8_t *entry = &buf[i * DIR_ENTRY_SIZE];
        for (int j = 0; j < MAX_FILENAME; j++) {
            directory[i].name[j] = entry[j];
            if (entry[j] == 0) break;
        }
        directory[i].size = *(uint32_t*)&entry[12];
        directory[i].start_sector = *(uint32_t*)&entry[16];
        directory[i].used = (directory[i].name[0] != 0);
    }
    return true;
}

// Монтирование (или создание новой) ФС
bool mount_fs() {
    if (!disk_init()) {
        print_string("Disk error!\n");
        return false;
    }
    if (read_superblock()) {
        // Диск уже отформатирован
        if (!read_directory()) return false;
    } else {
        // Форматируем
        print_string("Formatting disk...\n");
        write_superblock();
        for (int i = 0; i < MAX_FILES; i++) {
            directory[i].name[0] = 0;
            directory[i].size = 0;
            directory[i].start_sector = 0;
            directory[i].used = false;
        }
        write_directory();
    }
    fs_mounted = true;
    return true;
}

// Поиск свободной записи
static int find_free_entry() {
    for (int i = 0; i < MAX_FILES; i++) {
        if (!directory[i].used) return i;
    }
    return -1;
}

// Поиск файла по имени
static int find_file(const char* name) {
    for (int i = 0; i < MAX_FILES; i++) {
        if (directory[i].used && strcmp(directory[i].name, name) == 0)
            return i;
    }
    return -1;
}

// Выделение секторов (последовательное)
static uint32_t allocate_sectors(uint32_t num_sectors) {
    (void)num_sectors;
    // Ищем максимальный занятый сектор среди всех файлов
    uint32_t max_sector = DATA_SECTOR_START;
    for (int i = 0; i < MAX_FILES; i++) {
        if (directory[i].used) {
            uint32_t end = directory[i].start_sector + 
                (directory[i].size + SECTOR_SIZE - 1) / SECTOR_SIZE;
            if (end > max_sector) max_sector = end;
        }
    }
    uint32_t start = max_sector;
    // Обновляем «указатель» для следующего выделения (не используется, но пусть будет)
    // (можно сохранять в суперблок, но пока так)
    return start;
}

// ---- Команды оболочки ----
void cmd_help() {
    print_string("\nFatCatOS Shell Commands:\n");
    set_color(0x0E, 0x00);
    print_string("  help                 - show this help\n");
    print_string("  dir                  - list files\n");
    print_string("  create <name>        - create a file\n");
    print_string("  write <name> <data>  - write data to file\n");
    print_string("  read <name>          - display file contents\n");
    print_string("  del <name>           - delete file\n");
    print_string("  cldisk               - format disk\n");
    print_string("  cls                  - clear screen\n");
    print_string("  parrot <text>        - parrot mode\n");
    print_string("  color <fg> <bg>      - set colors\n");
    print_string("  exit                 - shutdown\n");
    set_color(0x07, 0x00);
    print_string("\nKeyboard Shortcuts:\n");
    set_color(0x0B, 0x00);
    print_string("  PageUp/PageDown      - scroll terminal\n");
    print_string("  Shift+Arrow Keys     - select text\n");
    print_string("  Ctrl+C               - copy selection\n");
    print_string("  Ctrl+V               - paste clipboard\n");
    print_string("  Ctrl+A               - select all\n");
    print_string("  Esc                  - clear selection\n\n");
    set_color(0x07, 0x00);
}

void cmd_dir() {
    if (!fs_mounted) { print_string("No disk.\n"); return; }
    print_string("Files in root:\n");
    int cnt = 0;
    for (int i = 0; i < MAX_FILES; i++) {
        if (directory[i].used) {
            print_string("  ");
            print_string(directory[i].name);
            // размер
            char buf[10];
            int sz = directory[i].size;
            int pos = 0;
            if (sz == 0) buf[pos++] = '0';
            else {
                while (sz) { buf[pos++] = '0' + sz % 10; sz /= 10; }
                for (int j = 0; j < pos/2; j++) { char t=buf[j]; buf[j]=buf[pos-1-j]; buf[pos-1-j]=t; }
            }
            buf[pos] = 0;
            print_string("  ");
            print_string(buf);
            print_string(" bytes\n");
            cnt++;
        }
    }
    if (cnt == 0) print_string("  (empty)\n");
    else {
        print_string("Total: ");
        char cs[4]; int n=cnt, p=0;
        if (n==0) cs[p++]='0'; else while(n){cs[p++]='0'+n%10; n/=10;}
        for(int j=0;j<p/2;j++){char t=cs[j]; cs[j]=cs[p-1-j]; cs[p-1-j]=t;}
        cs[p]=0;
        print_string(cs);
        print_string(" file(s)\n");
    }
}

void cmd_create(const char* name) {
    if (!fs_mounted) return;
    if (find_file(name) >= 0) {
        print_string("File already exists.\n");
        return;
    }
    int idx = find_free_entry();
    if (idx < 0) {
        print_string("Directory full (max 32 files).\n");
        return;
    }
    strcpy(directory[idx].name, name);
    directory[idx].size = 0;
    directory[idx].start_sector = allocate_sectors(0);
    directory[idx].used = true;
    write_directory();
    print_string("File created.\n");
}

void cmd_write(const char* name, const char* data) {
    if (!fs_mounted) return;
    int idx = find_file(name);
    if (idx < 0) {
        print_string("File not found.\n");
        return;
    }
    int len = strlen(data);
    uint32_t sectors_needed = (len + SECTOR_SIZE - 1) / SECTOR_SIZE;
    uint32_t new_start = allocate_sectors(sectors_needed);
    directory[idx].size = len;
    directory[idx].start_sector = new_start;
    write_directory();

    // Запись данных
    uint8_t buf[SECTOR_SIZE];
    for (uint32_t i = 0; i < sectors_needed; i++) {
        int offset = i * SECTOR_SIZE;
        int remaining = len - offset;
        int chunk = remaining < SECTOR_SIZE ? remaining : SECTOR_SIZE;
        for (int j = 0; j < chunk; j++) buf[j] = data[offset + j];
        for (int j = chunk; j < SECTOR_SIZE; j++) buf[j] = 0;
        if (!disk_write_sector(new_start + i, buf)) {
            print_string("Write error.\n");
            return;
        }
    }
    print_string("Written.\n");
}

void cmd_read(const char* name) {
    if (!fs_mounted) return;
    int idx = find_file(name);
    if (idx < 0) {
        print_string("File not found.\n");
        return;
    }
    uint32_t size = directory[idx].size;
    uint32_t sector = directory[idx].start_sector;
    uint8_t buf[SECTOR_SIZE];
    uint32_t read_bytes = 0;
    while (read_bytes < size) {
        if (!disk_read_sector(sector, buf)) {
            print_string("Read error.\n");
            return;
        }
        uint32_t to_read = size - read_bytes;
        if (to_read > SECTOR_SIZE) to_read = SECTOR_SIZE;
        for (uint32_t i = 0; i < to_read; i++) putchar(buf[i]);
        read_bytes += to_read;
        sector++;
    }
    print_string("\n");
}

void cmd_del(const char* name) {
    if (!fs_mounted) return;
    int idx = find_file(name);
    if (idx < 0) {
        print_string("File not found.\n");
        return;
    }
    directory[idx].used = false;
    directory[idx].name[0] = 0;
    write_directory();
    print_string("File deleted.\n");
}

void cmd_parrot(const char* text) {
    print_string("\xF0\x9F\xA6\x9C  ");  // эмодзи попугая (UTF-8)
    print_string(text);
    print_string("\n    Polly wants a cracker!\n");
}

// Разбор и выполнение команды
void execute_command(char* input) {
    while (*input == ' ') input++;
    if (*input == 0) return;

    char cmd[32] = {0};
    int i = 0;
    while (*input && *input != ' ' && i < 31) cmd[i++] = *input++;
    cmd[i] = 0;
    while (*input == ' ') input++;
    char* args = input;

    if (strcmp(cmd, "help") == 0) {
        cmd_help();
    } else if (strcmp(cmd, "dir") == 0) {
        cmd_dir();
    } else if (strcmp(cmd, "cldisk") == 0) {
        // Переформатировать диск (осторожно, данные теряются)
        write_superblock();
        for (int i = 0; i < MAX_FILES; i++) {
            directory[i].name[0] = 0;
            directory[i].used = false;
        }
        write_directory();
        print_string("Disk formatted.\n");
    } else if (strcmp(cmd, "create") == 0) {
        if (*args) cmd_create(args);
        else print_string("Usage: create <filename>\n");
    } else if (strcmp(cmd, "write") == 0) {
        // Разбор: write <file> <data>
        char filename[32];
        int j=0;
        while (*args && *args!=' ' && j<31) filename[j++] = *args++;
        filename[j]=0;
        if (*args==0) { print_string("Usage: write <file> <data>\n"); return; }
        while (*args==' ') args++;
        cmd_write(filename, args);
    } else if (strcmp(cmd, "read") == 0) {
        if (*args) cmd_read(args); else print_string("Usage: read <file>\n");
    } else if (strcmp(cmd, "del") == 0) {
        if (*args) cmd_del(args); else print_string("Usage: del <file>\n");
    } else if (strcmp(cmd, "cls") == 0) {
        clear_screen();
    } else if (strcmp(cmd, "parrot") == 0) {
        if (*args) cmd_parrot(args); else print_string("Parrot needs words!\n");
    } else if (strcmp(cmd, "color") == 0) {
        if (*args) {
            int fg = 0, bg = 0;
            while (*args >= '0' && *args <= '9') { fg = fg * 10 + (*args - '0'); args++; }
            while (*args == ' ') args++;
            while (*args >= '0' && *args <= '9') { bg = bg * 10 + (*args - '0'); args++; }
            if (fg >= 0 && fg <= 15 && bg >= 0 && bg <= 15) {
                set_color(fg, bg);
                print_string("Color set.\n");
            } else {
                print_string("Invalid color (0-15).\n");
            }
        } else {
            print_string("Usage: color <fg> <bg>\n");
            print_string("Colors: 0=black 1=blue 2=green 3=cyan 4=red 5=magenta 6=brown 7=gray\n");
            print_string("        8=dkgray 9=ltblue 10=ltgreen 11=ltcyan 12=ltred 13=ltmagenta 14=yellow 15=white\n");
        }
    } else if (strcmp(cmd, "exit") == 0) {
        // выход обрабатывается в main loop
    } else {
        print_string("Unknown command: "); print_string(cmd); print_string("\n");
    }
}

void shell_start() {
    if (!mount_fs()) {
        print_string("Failed to mount filesystem.\n");
        return;
    }
    set_color(0x0C, 0x00);
    print_string("FatCatOS Shell v1.0\n");
    set_color(0x07, 0x00);
    print_string("Type 'help' for commands. PageUp/Down to scroll.\n\n");

    char buffer[256];
    int pos = 0;

    while (1) {
        set_color(0x0A, 0x00);
        print_string("fatcat");
        set_color(0x08, 0x00);
        print_string("@");
        set_color(0x0C, 0x00);
        print_string("os");
        set_color(0x07, 0x00);
        print_string("> ");
        pos = 0;
        buffer[0] = 0;

        while (1) {
            unsigned char sc = get_scancode();

            // Обработка отпускания клавиш
            if (sc & 0x80) {
                if (sc == 0xAA || sc == 0xB6) shift_pressed = false;
                if (sc == 0x9D) ctrl_pressed = false;
                continue;
            }

            // Модификаторы
            if (sc == 0x2A || sc == 0x36) { shift_pressed = true; continue; }
            if (sc == 0x1D) { ctrl_pressed = true; continue; }

            // Ctrl+комбинации
            if (ctrl_pressed) {
                if (sc == 0x2E) { copy_selection(); continue; }  // Ctrl+C
                if (sc == 0x2F) { paste_clipboard(); continue; }  // Ctrl+V
                if (sc == 0x1E) { select_all(); continue; }       // Ctrl+A
                continue;
            }

            // Shift+стрелки для выделения
            if (shift_pressed) {
                if (sc == 0x48) { select_move(0, -1); continue; }   // Up
                if (sc == 0x50) { select_move(0, 1); continue; }    // Down
                if (sc == 0x4B) { select_move(-1, 0); continue; }   // Left
                if (sc == 0x4D) { select_move(1, 0); continue; }    // Right
                continue;
            }

            // Page Up/Down для скроллинга
            if (sc == 0x49) { scroll_up(); continue; }  // PageUp
            if (sc == 0x51) { scroll_down(); continue; } // PageDown

            // Esc - очистить выделение
            if (sc == 0x01) { select_clear(); continue; }

            if (sc == 0x1C) {  // Enter
                select_clear();
                buffer[pos] = 0;
                putchar('\n');
                break;
            }
            if (sc == 0x0E) {  // Backspace
                if (pos > 0) { pos--; putchar('\b'); }
                continue;
            }
            if (sc < sizeof(scancode_to_ascii)) {
                char c = scancode_to_ascii[sc];
                if (c && pos < (int)(sizeof(buffer)-1)) {
                    buffer[pos++] = c;
                    putchar(c);
                }
            }
        }

        if (strcmp(buffer, "exit") == 0) {
            print_string("Shutting down...\n");
            break;
        }

        if (pos > 0) execute_command(buffer);
    }
}