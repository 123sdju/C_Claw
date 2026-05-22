#ifndef CCLAW_STM32H743_BOARD_H
#define CCLAW_STM32H743_BOARD_H

void board_init(void);
void board_uart_write(const char *text);
int board_uart_getc_nonblocking(char *out);

#endif
