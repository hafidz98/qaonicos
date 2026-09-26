/* Minimal polled PL011 UART driver (qemu-system-arm -M virt, UART0). */
/* Pola yang sama dipakai nanti untuk UART 8250 RV1103 (polled TX). */

#define UART0_BASE 0x09000000u
#define UARTDR     (*(volatile unsigned int *)(UART0_BASE + 0x00u))
#define UARTFR     (*(volatile unsigned int *)(UART0_BASE + 0x18u))
#define FR_TXFF    (1u << 5)

static void uart_putc(char c)
{
    while (UARTFR & FR_TXFF) { /* tunggu sampai TX FIFO tidak penuh */ }
    UARTDR = (unsigned int)c;
}

static void uart_puts(const char *s)
{
    while (*s) {
        if (*s == '\n')
            uart_putc('\r');
        uart_putc(*s++);
    }
}

int main(void)
{
    uart_puts("Hello dari Cortex-A7 bare-metal!\n");
    uart_puts("Startup asm + linker script + UART polled: OK.\n");
    return 0;
}
