#ifndef LWIP_ARCH_CC_H
#define LWIP_ARCH_CC_H

#include <stdint.h>

void renode_uart_write(const char *text);

#define BYTE_ORDER LITTLE_ENDIAN
#define LWIP_PLATFORM_DIAG(x) do { } while (0)
#define LWIP_PLATFORM_ASSERT(x) do { renode_uart_write("[fail] lwip_assert: "); renode_uart_write(x); renode_uart_write("\n"); for (;;) { } } while (0)

#endif
