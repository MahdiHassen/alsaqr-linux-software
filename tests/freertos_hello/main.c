#include <FreeRTOS.h>
#include <task.h>
#include <stdint.h>

#include "uart/uart.h"
#include "hw_platform.h"

/* Bare-metal FAT16 + SDVB disparity workload (headers are macro-override-free). */
#include "sdvb_bench/fat16.h"
#include "sdvb_bench/fat_file.h"

extern void bare_malloc_reset( void );
extern int  sdvb_disparity( int argc, char *argv[] );

#define FPGA_UART_CLK_HZ   25000000u
#define FPGA_BAUD_RATE     9600u

#define PAD_MUX_REG_A      ((volatile int *) 0x1a10407C)
#define PAD_MUX_REG_B      ((volatile int *) 0x1a104084)

extern void freertos_risc_v_trap_handler( void );

/* CVA6 perf-counter event codes (5-bit mhpmevent selectors). See
 * hardware/ip_list/cva6/core/perf_counters.sv. This config implements
 * 6 generic counters (mhpmcounter3..8) plus fixed mcycle/minstret. */
#define EVT_L1_ICACHE_MISS   0x01
#define EVT_L1_DCACHE_MISS   0x02
#define EVT_LOAD             0x05
#define EVT_STORE            0x06
#define EVT_BRANCH           0x09
#define EVT_BRANCH_MISPRED   0x0A

#define READ_CSR( name ) ( { uint64_t __v; __asm__ volatile ( "csrr %0, " #name : "=r"( __v ) ); __v; } )
#define WRITE_CSR( name, val ) __asm__ volatile ( "csrw " #name ", %0" :: "r"( (uint64_t)( val ) ) )

static void prvSetupHardware( void )
{
    *PAD_MUX_REG_A = 1;
    *PAD_MUX_REG_B = 1;
    uart_set_cfg( 0, (uint16_t) ( ( FPGA_UART_CLK_HZ / FPGA_BAUD_RATE ) >> 4 ) );
}

static void u64_to_dec( uint64_t v, char *buf )
{
    char tmp[21];
    int i = 0;
    if( v == 0 ) { buf[0] = '0'; buf[1] = '\0'; return; }
    while( v > 0 ) { tmp[i++] = (char)( '0' + ( v % 10 ) ); v /= 10; }
    int j = 0;
    while( i > 0 ) { buf[j++] = tmp[--i]; }
    buf[j] = '\0';
}

static void print_kv( const char *label, uint64_t val )
{
    char num[24];
    u64_to_dec( val, num );
    uart_send_str( label );
    uart_send_str( num );
}

static void uart_send_u64( uint64_t v )
{
    char num[24];
    u64_to_dec( v, num );
    uart_send_str( num );
}

/* Only 4 HPCs are used now (counters 3..6); branch/mispred are left
 * unconfigured. Any 4 of the generic HPM events would serve equally well. */
static void configure_hpm( void )
{
    WRITE_CSR( mhpmevent3, EVT_L1_ICACHE_MISS );
    WRITE_CSR( mhpmevent4, EVT_L1_DCACHE_MISS );
    WRITE_CSR( mhpmevent5, EVT_LOAD );
    WRITE_CSR( mhpmevent6, EVT_STORE );
}

/* ---- 1 ms polling-overhead probe ---------------------------------------
 * configTICK_RATE_HZ is 1000, so the tick hook fires every 1 ms. We don't care
 * about the counter values, only what running the poll costs the benchmark: the
 * hook reads 4 HPCs into a sink when armed, and we time one disparity run with
 * it off and one with it on. SAMPLE_PERIOD_TICKS is the polling-period knob
 * (1 -> every 1 ms, 2 -> every 2 ms, ...). */
#define SAMPLE_PERIOD_TICKS  1u

static volatile uint64_t g_sink;        /* keeps the HPC reads from being optimised away */
static volatile uint32_t g_poll_count;  /* times the poll actually ran this pass */
static volatile uint32_t g_tick_div;    /* ticks -> period divider */
static volatile int      g_sampling_on;

/* One full disparity run on the sim-sized stereo pair. */

static FAT16 g_vol;

static void run_disparity_once( void )
{
    char *argv[] = { "disp", "DISP" };
    bare_malloc_reset();
    sdvb_disparity( 2, argv );
}

/* Time one disparity run. With polling==1 the tick hook reads 4 HPCs every
 * SAMPLE_PERIOD_TICKS ms during the run; with polling==0 it does nothing. */
static uint64_t timed_disparity_run( int polling )
{
    g_tick_div    = 0;
    g_poll_count  = 0;
    g_sampling_on = polling;

    uint64_t c0 = READ_CSR( mcycle );
    run_disparity_once();
    uint64_t c1 = READ_CSR( mcycle );

    g_sampling_on = 0;
    return c1 - c0;
}

static void print_overhead( uint64_t base, uint64_t polled, uint32_t polls )
{
    uint64_t delta = ( polled > base ) ? ( polled - base ) : 0;
    uart_send_str( "\r\n==== polling overhead (period=" );
    uart_send_u64( SAMPLE_PERIOD_TICKS );
    uart_send_str( " ms) ====\r\n" );
    print_kv( "cycles_no_poll=",  base );   uart_send_str( "\r\n" );
    print_kv( "cycles_polled=",   polled ); uart_send_str( "\r\n" );
    print_kv( "overhead_cycles=", delta );  uart_send_str( "\r\n" );
    print_kv( "polls=",           polls );  uart_send_str( "\r\n" );
    print_kv( "cycles_per_poll=", polls ? ( delta / polls ) : 0 ); uart_send_str( "\r\n" );
    uart_send_str( "==== done ====\r\n" );
}

static void prvRunTask( void *pvParameters )
{
    ( void ) pvParameters;

    configure_hpm();

    /* Warm-up run (discarded) so both measured runs see a warm cache, leaving
     * the poll as the only difference between them. */
    timed_disparity_run( 0 );

    uint64_t base   = timed_disparity_run( 0 );   /* no polling */
    uint64_t polled = timed_disparity_run( 1 );   /* poll every period */

    print_overhead( base, polled, g_poll_count );

    for( ;; ) { vTaskDelay( portMAX_DELAY ); }
}

int main( void )
{
    uint32_t mhartid;
    __asm__ volatile ( "csrr %0, mhartid" : "=r" ( mhartid ) );
    __asm__ volatile ( "csrw mtvec, %0" :: "r"( freertos_risc_v_trap_handler ) );

    /* Secondaries are intentionally left parked in the bootrom (not woken),
     * so hart 0 runs alone. Guard here in case one ever reaches main. */
    if( mhartid != 0 )
    {
        for( ;; ) { __asm__ volatile ( "wfi" ); }
    }

    prvSetupHardware();
    uart_send_str( "FreeRTOS boot (hart 0): SDVB disparity (sim) polling-overhead\r\n" );

    /* Mount the embedded FAT16 image so the disparity benchmark can read
     * DISP/1.bmp and DISP/2.bmp. */
    if( fat16_mount( &g_vol ) != 0 )
    {
        uart_send_str( "FATAL: fat16_mount failed\r\n" );
        for( ;; );
    }
    fat_file_init( &g_vol );

    xTaskCreate( prvRunTask,
                 "Run",
                 configMINIMAL_STACK_SIZE * 8,   /* disparity call tree */
                 NULL,
                 tskIDLE_PRIORITY + 1,
                 NULL );

    vTaskStartScheduler();

    for( ;; );
    return 0;
}

/* Fires every 1 ms (configTICK_RATE_HZ = 1000). When armed, every
 * SAMPLE_PERIOD_TICKS-th tick reads the 4 HPCs into a sink — this is the work
 * whose effect on the benchmark we measure. Runs in the timer ISR: CSR reads
 * only, no UART / no blocking. */
void vApplicationTickHook( void )
{
    if( !g_sampling_on ) return;
    if( ++g_tick_div < SAMPLE_PERIOD_TICKS ) return;
    g_tick_div = 0;

    g_sink += READ_CSR( mhpmcounter3 )
            + READ_CSR( mhpmcounter4 )
            + READ_CSR( mhpmcounter5 )
            + READ_CSR( mhpmcounter6 );
    g_poll_count++;
}
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
