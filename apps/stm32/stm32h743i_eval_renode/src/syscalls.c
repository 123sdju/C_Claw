#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>

#define STM32_USART3_BASE 0x40004800UL
#define USART_CR1 0x00U
#define USART_ISR 0x1CU
#define USART_RDR 0x24U
#define USART_TDR 0x28U
#define USART_CR1_UE (1U << 0)
#define USART_CR1_RE (1U << 2)
#define USART_CR1_TE (1U << 3)
#define USART_ISR_RXNE (1U << 5)
#define USART_ISR_TXE (1U << 7)

static volatile uint32_t *usart_reg(uint32_t offset)
{
    return (volatile uint32_t *)(STM32_USART3_BASE + offset);
}

void renode_uart_init(void)
{
    *usart_reg(USART_CR1) = USART_CR1_UE | USART_CR1_TE | USART_CR1_RE;
}

void renode_uart_putc(char ch)
{
    while ((*usart_reg(USART_ISR) & USART_ISR_TXE) == 0U) {
    }
    *usart_reg(USART_TDR) = (uint32_t)(uint8_t)ch;
}

void renode_uart_write(const char *text)
{
    if (!text) return;
    while (*text) {
        if (*text == '\n') renode_uart_putc('\r');
        renode_uart_putc(*text++);
    }
}

int renode_uart_getc_nonblocking(char *out)
{
    if (!out) return 0;
    if ((*usart_reg(USART_ISR) & USART_ISR_RXNE) == 0U) return 0;
    *out = (char)(uint8_t)(*usart_reg(USART_RDR));
    return 1;
}

extern char __heap_start__;
extern char __heap_end__;
static char *heap_current = &__heap_start__;

void *_sbrk(ptrdiff_t incr)
{
    char *prev = heap_current;
    if (heap_current + incr > &__heap_end__) {
        errno = ENOMEM;
        return (void *)-1;
    }
    heap_current += incr;
    return prev;
}

int _write(int fd, const char *buf, int len)
{
    (void)fd;
    for (int i = 0; i < len; i++) renode_uart_putc(buf[i]);
    return len;
}

int _close(int fd) { (void)fd; return -1; }
int _fstat(int fd, struct stat *st) { (void)fd; st->st_mode = S_IFCHR; return 0; }
int _isatty(int fd) { (void)fd; return 1; }
int _lseek(int fd, int ptr, int dir) { (void)fd; (void)ptr; (void)dir; return 0; }
int _read(int fd, char *buf, int len)
{
    (void)fd;
    if (!buf || len <= 0) return 0;
    while (!renode_uart_getc_nonblocking(&buf[0])) {
    }
    return 1;
}
void _exit(int status) { (void)status; for (;;) {} }
int _kill(int pid, int sig) { (void)pid; (void)sig; errno = EINVAL; return -1; }
int _getpid(void) { return 1; }
