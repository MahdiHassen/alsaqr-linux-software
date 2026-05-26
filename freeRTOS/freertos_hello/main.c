#include <FreeRTOS.h>
#include <task.h>
#include <stdint.h>

#include "uart/uart.h"
#include "hw_platform.h"

#define MTIME_ADDR         ( CLINT_BASE + MTIME_BASE_ADDRESS )

#define FPGA_UART_CLK_HZ   25000000u
#define FPGA_BAUD_RATE     9600u

#define PAD_MUX_REG_A      ((volatile int *) 0x1a10407C)
#define PAD_MUX_REG_B      ((volatile int *) 0x1a104084)

/* he-soc bootrom wake-up: bootrom polls PLIC claim for source 10 (mailbox IRQ).
 * The PLIC setup (priority + enable) is mandatory — the IPI trigger alone does
 * not deliver an interrupt. Empirically this sequence wakes all CVA6 secondaries
 * (1, 2, 3 in QUAD_CORE) simultaneously, not just hart 1.
 * Mirrors software/common/syscalls.c:_init under FPGA_EMULATION. */
#define PLIC_PRIO_SRC10        ((volatile int *) 0x0C000028)
#define PLIC_ENABLE_HART0_M    ((volatile int *) 0x0C002080)
#define PLIC_ENABLE_HART1_M    ((volatile int *) 0x0C002180)
#define MBOX_BOOT_VECTOR       ((volatile int *) 0x10404000)
#define MBOX_IPI_TRIGGER       ((volatile int *) 0x10404024)

extern void freertos_risc_v_trap_handler( void );

static volatile uint32_t uart_lock  = 0;
static volatile uint32_t uart_ready = 0;

static inline void uart_acquire( void )
{
    while( __sync_lock_test_and_set( &uart_lock, 1u ) ) { /* spin */ }
}
static inline void uart_release( void )
{
    __sync_lock_release( &uart_lock );
}
static void uart_print_locked( const char *s )
{
    uart_acquire();
    uart_send_str( s );
    uart_release();
}

static inline uint64_t mtime_read( void )
{
    return *(volatile uint64_t *) MTIME_ADDR;
}
static void mtime_busy_delay( uint64_t ticks )
{
    uint64_t start = mtime_read();
    while( ( mtime_read() - start ) < ticks ) { /* spin */ }
}

static void prvSetupHardware( void )
{
    *PAD_MUX_REG_A = 1;
    *PAD_MUX_REG_B = 1;
    uart_set_cfg( 0, (uint16_t) ( ( FPGA_UART_CLK_HZ / FPGA_BAUD_RATE ) >> 4 ) );
}

static void prvHelloTask( void *pvParameters )
{
    ( void ) pvParameters;
    for( ;; )
    {
        uart_print_locked( "Hello from hart 0\r\n" );
        vTaskDelay( pdMS_TO_TICKS( 1000 ) );
    }
}

static void secondary_hello_loop( uint32_t hartid )
{
    char msg[] = "Hello from hart X\r\n";
    msg[16] = (char) ( '0' + hartid );

    const uint64_t one_sec = (uint64_t) RTC_FREQ;
    for( ;; )
    {
        uart_print_locked( msg );
        mtime_busy_delay( one_sec );
    }
}

int main( void )
{
    uint32_t mhartid;
    __asm__ volatile ( "csrr %0, mhartid" : "=r" ( mhartid ) );
    __asm__ volatile ( "csrw mtvec, %0" :: "r"( freertos_risc_v_trap_handler ) );

    if( mhartid != 0 )
    {
        while( !uart_ready ) { /* spin */ }
        __sync_synchronize();
        secondary_hello_loop( mhartid );
        for( ;; );
    }

    prvSetupHardware();
    uart_send_str( "FreeRTOS boot (multi-hart)\r\n" );
    __sync_synchronize();
    uart_ready = 1;

    *PLIC_PRIO_SRC10     = 0x1;
    *PLIC_ENABLE_HART0_M = 0x0;
    *PLIC_ENABLE_HART1_M = 0x400;
    *MBOX_BOOT_VECTOR    = 0x80000000;
    __sync_synchronize();
    *MBOX_IPI_TRIGGER    = 1;

    xTaskCreate( prvHelloTask,
                 "Hello",
                 configMINIMAL_STACK_SIZE,
                 NULL,
                 tskIDLE_PRIORITY + 1,
                 NULL );

    vTaskStartScheduler();

    for( ;; );
    return 0;
}

void vApplicationTickHook( void ) {}
void vApplicationIdleHook( void ) {}

void vApplicationMallocFailedHook( void )
{
    taskDISABLE_INTERRUPTS();
    __asm volatile( "ebreak" );
    for( ;; );
}

void vApplicationStackOverflowHook( TaskHandle_t pxTask, char *pcTaskName )
{
    ( void ) pxTask;
    ( void ) pcTaskName;
    taskDISABLE_INTERRUPTS();
    __asm volatile( "ebreak" );
    for( ;; );
}
