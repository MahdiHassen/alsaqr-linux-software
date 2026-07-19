#ifndef SDVB_INSTRUMENT_H
#define SDVB_INSTRUMENT_H

#include <stdint.h>

#define FD_TRACE_NUM_EVT  8
#define FD_TRACE_CAPACITY 256   /* expect ~8 calls * 11 runs = 88; comfortable headroom */

typedef struct
{
    uint16_t run;
    uint16_t call;
    /* delta = exit_snap - entry_snap for: cycles, instret, ic_miss, dc_miss,
     *                                     loads, stores, branches, br_mispred */
    uint64_t delta[ FD_TRACE_NUM_EVT ];
} fd_trace_entry_t;

void findDisparity_track_reset( void );           /* clear the whole trace buffer */
void findDisparity_track_set_run( uint16_t run ); /* call before each sweep run */
uint16_t fd_trace_size( void );
const fd_trace_entry_t *fd_trace_entry( uint16_t idx );

#endif
