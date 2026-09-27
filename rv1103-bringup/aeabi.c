/* aeabi.c - pembungkus divisi 32-bit di atas __aeabi_uldivmod (assembly).
 *
 * Hanya memakai aritmetika 32-bit di sini supaya clang tidak menurunkan
 * operasi ini menjadi panggilan __aeabi_* lain (rekursi tak berujung).
 */

extern unsigned long long __aeabi_uldivmod(unsigned long long n,
                                           unsigned long long d);

unsigned int __aeabi_uidiv(unsigned int n, unsigned int d)
{
    return (unsigned int)__aeabi_uldivmod(n, d);
}

int __aeabi_idiv(int n, int d)
{
    unsigned int un = (n < 0) ? (0u - (unsigned int)n) : (unsigned int)n;
    unsigned int ud = (d < 0) ? (0u - (unsigned int)d) : (unsigned int)d;
    int q = (int)__aeabi_uidiv(un, ud);
    return ((n < 0) ^ (d < 0)) ? -q : q;
}
