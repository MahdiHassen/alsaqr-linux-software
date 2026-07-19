#include "uart.h"

void uart_set_cfg(int parity, uint16_t clk_counter) {
    (void) parity;
    *(volatile unsigned int*)(UART_REG_LCR) = 0x83;
    *(volatile unsigned int*)(UART_REG_DLM) = (clk_counter >> 8) & 0xFF;
    *(volatile unsigned int*)(UART_REG_DLL) =  clk_counter       & 0xFF;
    *(volatile unsigned int*)(UART_REG_FCR) = 0xA7;
    *(volatile unsigned int*)(UART_REG_LCR) = 0x03;
    *(volatile unsigned int*)(UART_REG_IER) =
        ((*(volatile unsigned int*)(UART_REG_IER)) & 0xF0) | 0x02;
}

void uart_sendchar(char c) {
    while( (*(volatile unsigned int*)(UART_REG_LSR) & 0x20) == 0 );
    *(volatile unsigned int*)(UART_REG_THR) = (unsigned char) c;
}

void uart_send(const char* str, unsigned int len) {
    unsigned int i;
    while(len > 0) {
        while( (*(volatile unsigned int*)(UART_REG_LSR) & 0x20) == 0 );
        for(i = 0; (i < UART_FIFO_DEPTH) && (len > 0); i++) {
            *(volatile unsigned int*)(UART_REG_THR) = (unsigned char) *str++;
            len--;
        }
    }
}

void uart_send_str(const char* str) {
    while (*str) uart_sendchar(*str++);
}
