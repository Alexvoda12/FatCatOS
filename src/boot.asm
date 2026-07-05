; boot.asm - Multiboot загрузчик
section .multiboot
    align 4
    dd 0x1BADB002          ; Магическое число
    dd 0x03                ; Флаги
    dd -(0x1BADB002 + 0x03) ; Контрольная сумма

section .text
    global start
    extern kernel_main     ; Функция из kernel.c

start:
    mov esp, stack_top     ; Установка стека
    call kernel_main       ; Вызов ядра
    hlt                    ; Остановка если вернемся

section .bss
    align 16
stack_bottom:
    resb 16384             ; 16 KB стек
stack_top: