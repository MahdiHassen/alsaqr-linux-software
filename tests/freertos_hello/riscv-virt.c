#include <FreeRTOS.h>
#include "riscv-virt.h"
#include "uart/uart.h"

int xGetCoreID( void )
{
    int id;
    __asm__ volatile ( "csrr %0, mhartid" : "=r" ( id ) );
    return id;
}

void vSendString( const char *s )
{
    portENTER_CRITICAL();
    uart_send_str( s );
    portEXIT_CRITICAL();
}

void handle_trap( void )
{
    for( ;; );
}
