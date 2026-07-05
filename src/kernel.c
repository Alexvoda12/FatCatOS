// kernel.c - Ядро FatCatOS
#include <stdbool.h>
#include <stdint.h>

#define VIDEO_MEMORY 0xB8000
#define VGA_WIDTH 80
#define VGA_HEIGHT 25
#define SCROLLBACK 200

static uint16_t* vga = (uint16_t*)VIDEO_MEMORY;
static int cx = 0, cy = 0;
static uint8_t color = 0x07;

// Скроллинг: сохраняем строки
static uint16_t history[SCROLLBACK][VGA_WIDTH];
static int hist_lines = 0;     // сколько строк записано
static int hist_view = 0;      // 0 = показываем текущий экран, >0 = скроллим назад

// Выделение
static int sel_sx = -1, sel_sy = -1;
static int sel_ex = -1, sel_ey = -1;

bool shift_pressed = false;
bool ctrl_pressed = false;

extern void shell_start();

// --- VGA ---

static void vga_write(int x, int y, uint16_t ch) {
    if (x >= 0 && x < VGA_WIDTH && y >= 0 && y < VGA_HEIGHT)
        vga[y * VGA_WIDTH + x] = ch;
}

static uint16_t vga_read(int x, int y) {
    if (x >= 0 && x < VGA_WIDTH && y >= 0 && y < VGA_HEIGHT)
        return vga[y * VGA_WIDTH + x];
    return 0;
}

// Сохраняем текущую строку в историю
static void save_line(int y) {
    if (hist_lines >= SCROLLBACK) {
        // Сдвигаем вверх
        for (int i = 0; i < SCROLLBACK - 1; i++)
            for (int x = 0; x < VGA_WIDTH; x++)
                history[i][x] = history[i + 1][x];
        hist_lines = SCROLLBACK - 1;
    }
    for (int x = 0; x < VGA_WIDTH; x++)
        history[hist_lines][x] = vga_read(x, y);
    hist_lines++;
}

// Показать строку из истории на экране
static void show_hist_line(int hist_idx, int screen_y) {
    for (int x = 0; x < VGA_WIDTH; x++) {
        uint16_t ch = 0;
        if (hist_idx >= 0 && hist_idx < hist_lines)
            ch = history[hist_idx][x];
        vga_write(x, screen_y, ch);
    }
}

// Проверка в выделении
static bool in_sel(int x, int abs_y) {
    if (sel_sx < 0) return false;
    int sy1 = sel_sy, sy2 = sel_ey;
    int sx1 = sel_sx, sx2 = sel_ex;
    if (sy1 > sy2 || (sy1 == sy2 && sx1 > sx2)) {
        int t; t = sy1; sy1 = sy2; sy2 = t;
        t = sx1; sx1 = sx2; sx2 = t;
    }
    if (abs_y < sy1 || abs_y > sy2) return false;
    if (abs_y == sy1 && x < sx1) return false;
    if (abs_y == sy2 && x > sx2) return false;
    return true;
}

// Перерисовать выделение на экране (перекрасить фон)
static void redraw_selection() {
    if (sel_sx < 0) return;
    // Абсолютные координаты выделения
    int sy1 = sel_sy, sy2 = sel_ey;
    int sx1 = sel_sx, sx2 = sel_ex;
    if (sy1 > sy2 || (sy1 == sy2 && sx1 > sx2)) {
        int t; t = sy1; sy1 = sy2; sy2 = t;
        t = sx1; sx1 = sx2; sx2 = t;
    }
    // Какие строки видны на экране
    int top_abs = 0;
    if (hist_view > 0) {
        top_abs = hist_lines - hist_view;
    }
    for (int vy = 0; vy < VGA_HEIGHT; vy++) {
        int abs_y = top_abs + vy;
        for (int x = 0; x < VGA_WIDTH; x++) {
            if (in_sel(x, abs_y)) {
                uint16_t ch = vga_read(x, vy);
                uint8_t bg = 0x70; // инверсия
                vga_write(x, vy, (bg << 8) | (ch & 0xFF));
            }
        }
    }
}

// --- Терминал ---

void clear_screen() {
    for (int y = 0; y < VGA_HEIGHT; y++)
        for (int x = 0; x < VGA_WIDTH; x++)
            vga_write(x, y, (color << 8) | ' ');
    cx = 0;
    cy = 0;
}

void scroll_up() {
    // Сохраняем текущий экран в историю, если мы внизу
    if (hist_view == 0) {
        // Сохраняем все строки экрана
        for (int y = 0; y < VGA_HEIGHT; y++)
            save_line(y);
        hist_view = 1;
    }
    if (hist_view < hist_lines - 1)
        hist_view++;
    // Показываем из истории
    int base = hist_lines - hist_view;
    for (int vy = 0; vy < VGA_HEIGHT; vy++) {
        int hi = base + vy;
        show_hist_line(hi, vy);
    }
}

void scroll_down() {
    if (hist_view <= 0) return;
    hist_view--;
    if (hist_view == 0) {
        // Вернуться на текущий экран — нужно перерисовать из scrollback
        // Текущий экран был сохранён, восстанавливаем из истории
        int base = hist_lines - VGA_HEIGHT;
        if (base < 0) base = 0;
        for (int vy = 0; vy < VGA_HEIGHT; vy++)
            show_hist_line(base + vy, vy);
    } else {
        int base = hist_lines - hist_view;
        for (int vy = 0; vy < VGA_HEIGHT; vy++)
            show_hist_line(base + vy, vy);
    }
}

void backspace() {
    if (cx > 0) {
        cx--;
        vga_write(cx, cy, (color << 8) | ' ');
    }
}

void putchar(char c) {
    // Если смотрим историю — вернуться вниз
    if (hist_view > 0) {
        hist_view = 0;
        int base = (hist_lines > VGA_HEIGHT) ? hist_lines - VGA_HEIGHT : 0;
        for (int vy = 0; vy < VGA_HEIGHT; vy++)
            show_hist_line(base + vy, vy);
    }

    if (c == '\n') {
        // Сохраняем текущую строку в историю
        save_line(cy);
        cx = 0;
        cy++;
        if (cy >= VGA_HEIGHT) {
            // Скроллинг вверх
            for (int y = 0; y < VGA_HEIGHT - 1; y++)
                for (int x = 0; x < VGA_WIDTH; x++)
                    vga_write(x, y, vga_read(x, y + 1));
            for (int x = 0; x < VGA_WIDTH; x++)
                vga_write(x, VGA_HEIGHT - 1, (color << 8) | ' ');
            cy = VGA_HEIGHT - 1;
        }
        return;
    }
    if (c == '\b') {
        backspace();
        return;
    }
    vga_write(cx, cy, (color << 8) | c);
    cx++;
    if (cx >= VGA_WIDTH) {
        cx = 0;
        cy++;
        if (cy >= VGA_HEIGHT) {
            for (int y = 0; y < VGA_HEIGHT - 1; y++)
                for (int x = 0; x < VGA_WIDTH; x++)
                    vga_write(x, y, vga_read(x, y + 1));
            for (int x = 0; x < VGA_WIDTH; x++)
                vga_write(x, VGA_HEIGHT - 1, (color << 8) | ' ');
            cy = VGA_HEIGHT - 1;
        }
    }
}

void print_string(const char* s) {
    while (*s) putchar(*s++);
}

// --- Клавиатура ---

unsigned char inb(unsigned short port) {
    unsigned char r;
    __asm__("in %%dx, %%al" : "=a"(r) : "d"(port));
    return r;
}

char get_scancode() {
    while (!(inb(0x64) & 0x01));
    return inb(0x60);
}

// --- Выделение текста ---

void select_start() {
    sel_sx = cx;
    sel_sy = cy;
    sel_ex = cx;
    sel_ey = cy;
}

void select_move(int dx, int dy) {
    sel_ex += dx;
    sel_ey += dy;
    if (sel_ex < 0) sel_ex = 0;
    if (sel_ex >= VGA_WIDTH) sel_ex = VGA_WIDTH - 1;
    if (sel_ey < 0) sel_ey = 0;
    if (sel_ey >= VGA_HEIGHT) sel_ey = VGA_HEIGHT - 1;
    // Перерисовать экран с выделением
    redraw_selection();
}

void select_clear() {
    sel_sx = sel_sy = sel_ex = sel_ey = -1;
    // Перерисовать весь экран без выделения
    if (hist_view > 0) {
        int base = hist_lines - hist_view;
        for (int vy = 0; vy < VGA_HEIGHT; vy++)
            show_hist_line(base + vy, vy);
    }
}

static char clip_buf[4096];
void copy_selection() {
    if (sel_sx < 0) return;
    int sy1 = sel_sy, sy2 = sel_ey;
    int sx1 = sel_sx, sx2 = sel_ex;
    if (sy1 > sy2 || (sy1 == sy2 && sx1 > sx2)) {
        int t; t = sy1; sy1 = sy2; sy2 = t;
        t = sx1; sx1 = sx2; sx2 = t;
    }
    int pos = 0;
    for (int y = sy1; y <= sy2 && pos < 4095; y++) {
        for (int x = (y == sy1 ? sx1 : 0); x <= (y == sy2 ? sx2 : VGA_WIDTH - 1) && pos < 4095; x++) {
            uint16_t ch = vga_read(x, y);
            char c = ch & 0xFF;
            if (c != ' ' || x < sx2) clip_buf[pos++] = c;
        }
        if (y < sy2) clip_buf[pos++] = '\n';
    }
    clip_buf[pos] = '\0';
    select_clear();
}

void paste_clipboard() {
    for (int i = 0; clip_buf[i]; i++) putchar(clip_buf[i]);
}

void select_all() {
    sel_sx = 0;
    sel_sy = 0;
    sel_ex = VGA_WIDTH - 1;
    sel_ey = VGA_HEIGHT - 1;
    redraw_selection();
}

// --- Цвет ---

void set_color(uint8_t fg, uint8_t bg) {
    color = (bg << 4) | (fg & 0x0F);
}

// --- Точка входа ---

void kernel_main() {
    clear_screen();
    set_color(0x0A, 0x00);
    print_string(" ______              _                     ___  ____\n");
    print_string("|  ____|            | |                   / _ |/ ___|\n");
    print_string("| |__ __ _ _ __   __| | ___  _ __ ___   | | | |    \n");
    print_string("|  __/ _` | '_ \\ / _` |/ _ \\| '_ ` _ \\  | |_| |___ \n");
    print_string("| | | (_| | | | | (_| | (_) | | | | | |  \\___/\\___|\n");
    print_string("|_|  \\__,_|_| |_|\\__,_|\\___/|_| |_| |_|        ____\n");
    print_string("                                              |____|\n\n");
    set_color(0x07, 0x00);
    print_string("FatCatOS v1.0\n");
    print_string("Type 'help' for commands.\n");
    print_string("Scroll: PageUp/PageDown | Select: Shift+Arrows\n");
    print_string("Copy: Ctrl+C | Paste: Ctrl+V | Select All: Ctrl+A\n\n");
    shell_start();
    clear_screen();
    print_string("System halted.\n");
    while(1) { __asm__("hlt"); }
}
