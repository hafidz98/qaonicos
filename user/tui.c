/*
 * tui.c - Implementasi tui.h (Fase 17).
 *
 * Bare-metal, tanpa libc. Satu-satunya primitif output = u_write(1).
 * Frame dirakit di buffer statis 2KB lalu di-flush dalam satu
 * syscall SYS_WRITE (pola sys_write_user: IRQ di-mask selama tulis,
 * jadi satu frame tak terpotong thread lain).
 */
#include "tui.h"
#include "ulib/ulib.h"

#define TUI_OB_SZ 2048u

static char ob[TUI_OB_SZ];
static unsigned on;

static void emit_c(char c)
{
    if (on < TUI_OB_SZ - 1u)
        ob[on++] = c;
}

static void emit_s(const char *s)
{
    while (s && *s)
        emit_c(*s++);
}

static void emit_u(unsigned v)
{
    char tmp[10];
    int n = 0, k;
    if (v == 0u) {
        emit_c('0');
        return;
    }
    while (v > 0u && n < 10) {
        tmp[n++] = (char)('0' + v % 10u);
        v /= 10u;
    }
    for (k = n - 1; k >= 0; k--)
        emit_c(tmp[k]);
}

void tui_begin(void)
{
    on = 0u;
}

void tui_flush(void)
{
    if (on > 0u)
        u_write(1u, ob, on);
    on = 0u;
}

void tui_clear(void)
{
    emit_s("\x1b[2J\x1b[H");
}

void tui_goto(unsigned r, unsigned c)
{
    emit_s("\x1b[");
    emit_u(r);
    emit_c(';');
    emit_u(c);
    emit_c('H');
}

void tui_puts(const char *s)
{
    emit_s(s);
}

void tui_putc(char c)
{
    emit_c(c);
}

void tui_putu(unsigned v)
{
    emit_u(v);
}

void tui_at(unsigned r, unsigned c, const char *s)
{
    tui_goto(r, c);
    emit_s(s);
}

void tui_box(unsigned r, unsigned c, unsigned w, unsigned h,
             const char *title)
{
    unsigned i, j, tn = 0u;

    if (w < 4u || h < 3u)
        return;
    while (title && title[tn])
        tn++;
    /* Sisi atas. */
    tui_goto(r, c);
    emit_c('+');
    for (i = 0u; i < w - 2u; i++)
        emit_c('-');
    emit_c('+');
    /* Judul menimpa sisi atas bila muat. */
    if (tn > 0u && tn + 4u < w) {
        tui_goto(r, c + 2u);
        emit_c(' ');
        emit_s(title);
        emit_c(' ');
    }
    /* Sisi kiri/kanan. */
    for (j = 1u; j < h - 1u; j++) {
        tui_goto(r + j, c);
        emit_c('|');
        tui_goto(r + j, c + w - 1u);
        emit_c('|');
    }
    /* Sisi bawah. */
    tui_goto(r + h - 1u, c);
    emit_c('+');
    for (i = 0u; i < w - 2u; i++)
        emit_c('-');
    emit_c('+');
}

void tui_bar(unsigned w, unsigned pct)
{
    unsigned inner, fill, i;

    if (w < 3u)
        return;
    if (pct > 100u)
        pct = 100u;
    inner = w - 2u;
    fill = (inner * pct) / 100u;
    emit_c('[');
    for (i = 0u; i < inner; i++)
        emit_c(i < fill ? '#' : '-');
    emit_c(']');
}

void tui_status(unsigned r, const char *s)
{
    unsigned n = 0u;

    tui_goto(r, 1u);
    emit_s("\x1b[7m");   /* video terbalik */
    emit_s(s);
    while (s && s[n])
        n++;
    while (n < 80u) {    /* penuhi selebar layar */
        emit_c(' ');
        n++;
    }
    emit_s("\x1b[0m");
}
