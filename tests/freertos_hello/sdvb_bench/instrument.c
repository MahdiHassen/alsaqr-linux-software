/*
 * -finstrument-functions hooks for findDisparity(). On entry we snapshot all 8
 * HPM counters; on exit we record (delta, run, call#) into a flat trace buffer.
 *
 * Only findDisparity.c is built with -finstrument-functions, so these hooks
 * only fire for findDisparity (the address filter keeps the tracker focused if
 * more files get instrumented later). Everything here is no_instrument_function
 * or it would recurse.
 */
#include "instrument.h"

/* Address-only declaration; we never call through this pointer, only compare. */
extern void findDisparity();

#define READ_CSR( name ) \
    ( { uint64_t __v; __asm__ volatile ( "csrr %0, " #name : "=r"( __v ) ); __v; } )

static uint64_t enter_snap[ FD_TRACE_NUM_EVT ];
static int      in_target;        /* recursion / unbalanced-call guard */

static fd_trace_entry_t g_trace[ FD_TRACE_CAPACITY ];
static uint16_t         g_count;
static uint16_t         g_run;
static uint16_t         g_call_in_run;

__attribute__((no_instrument_function))
static inline void read_all( uint64_t *out )
{
    out[0] = READ_CSR( mcycle );
    out[1] = READ_CSR( minstret );
    out[2] = READ_CSR( mhpmcounter3 );   /* L1 I$ miss   */
    out[3] = READ_CSR( mhpmcounter4 );   /* L1 D$ miss   */
    out[4] = READ_CSR( mhpmcounter5 );   /* loads        */
    out[5] = READ_CSR( mhpmcounter6 );   /* stores       */
    out[6] = READ_CSR( mhpmcounter7 );   /* branches     */
    out[7] = READ_CSR( mhpmcounter8 );   /* br mispredict*/
}

__attribute__((no_instrument_function))
void __cyg_profile_func_enter( void *this_fn, void *call_site )
{
    ( void ) call_site;
    if( this_fn != (void *) &findDisparity ) return;
    if( in_target ) return;
    in_target = 1;
    read_all( enter_snap );
}

__attribute__((no_instrument_function))
void __cyg_profile_func_exit( void *this_fn, void *call_site )
{
    ( void ) call_site;
    if( this_fn != (void *) &findDisparity ) return;
    if( !in_target ) return;

    uint64_t now[ FD_TRACE_NUM_EVT ];
    read_all( now );

    if( g_count < FD_TRACE_CAPACITY )
    {
        fd_trace_entry_t *e = &g_trace[ g_count++ ];
        e->run  = g_run;
        e->call = g_call_in_run;
        for( int i = 0; i < FD_TRACE_NUM_EVT; i++ )
            e->delta[ i ] = now[ i ] - enter_snap[ i ];
    }
    g_call_in_run++;
    in_target = 0;
}

__attribute__((no_instrument_function))
void findDisparity_track_reset( void )
{
    g_count = 0;
    g_run = 0;
    g_call_in_run = 0;
}

__attribute__((no_instrument_function))
void findDisparity_track_set_run( uint16_t run )
{
    g_run = run;
    g_call_in_run = 0;
}

__attribute__((no_instrument_function))
uint16_t fd_trace_size( void )
{
    return g_count;
}

__attribute__((no_instrument_function))
const fd_trace_entry_t *fd_trace_entry( uint16_t idx )
{
    return ( idx < g_count ) ? &g_trace[ idx ] : (const fd_trace_entry_t *) 0;
}
