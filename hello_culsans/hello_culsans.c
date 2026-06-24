//#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "utils.h"
#include "encoding.h"

#define UART_BASE        0x40000000u
#define UART_DLL         (*(volatile uint8_t *)(UART_BASE + 0x00)) // also RBR/THR
#define UART_DLM         (*(volatile uint8_t *)(UART_BASE + 0x04)) // also IER
#define UART_LCR         (*(volatile uint8_t *)(UART_BASE + 0x0C))

#define pulp_read32(add) (*(volatile unsigned int *)(long)(add))

static inline void uart_program_early(void) {
  // (gdb) set {int}0x4000000C = 0x83    # Set DLAB=1 (and word length=8)
  UART_LCR = 0x83;
  __asm__ volatile("fence iorw, iorw");

  // (gdb) set {char}0x40000000 = 0x1B   # DLL = 27
  //UART_DLL = 0x1B;
  //__asm__ volatile("fence iorw, iorw");

  // (gdb) set {char}0x40000004 = 0x00   # DLM = 0
  //UART_DLM = 0x00;
  //__asm__ volatile("fence iorw, iorw");

  // (gdb) set {int}0x4000000C = 0x03    # Clear DLAB, set 8N1
  UART_LCR = 0x03;
  __asm__ volatile("fence iorw, iorw");
}

__attribute__ ((section(".heapl2ram"))) volatile int flag[2] = { 0, 0};
__attribute__ ((section(".heapl2ram"))) volatile int victim = -1;
__attribute__ ((section(".heapl2ram"))) volatile int core_1 = -1;

static __attribute__ ((noinline)) void lock(int cid) {
  flag[cid] = 1;
  victim = cid;
  int other_cid = 1 - cid;
  while (flag[other_cid] && (victim == cid)) {
    __asm__ volatile("nop;");
  }
}

static __attribute__ ((noinline)) void unlock(int cid) {
  flag[cid] = 0;
}

int thread_entry(int cid, int nc) {
  // Program UART ASAP; do it once (on core 0) to avoid concurrent reprogramming.
  if (cid == 0) {
    uart_program_early();
  }

  lock(cid);
  uint64_t misa = read_csr(misa);
  if (!(misa & 0x80)) {
    printf("Hypervisor extension MISSING!\r\n");
    uart_wait_tx_done();
    unlock(cid);
    return -1;
  }
  uart_program_early();

  if(cid == 0){

      printf("Hello Mahdi");
            printf("Hello Fred");
      printf("Hello Emon");
            printf("Hello Rodolfo");
                  printf("Hello Ryan");
                        printf("Hello Sara\n");

                              // printf("Hello Ali\n");

      *((volatile uint32_t *)0x85000000) = 0x1234ABCD;

                              // while (1){
                              //   printf("UHHHH UMMMM HNMMM IDK MAN THE APD IS KILLING ME HERE SO MANY SIGNALS I CANT BREATH Um maybe i can\n\n");

                              //   printf("Ash is more than just the gray powdery substance left after combustion—it is a symbol of transformation, renewal, and the enduring cycle of life and decay. Formed when organic material such as wood, plants, or even volcanic rock undergoes burning or intense heat, ash represents what remains when all that is transient is consumed.\n\n");


                              //   printf("In nature, ash plays a vital role in regeneration. After forest fires, for instance, ash enriches the soil with minerals such as calcium, potassium, and magnesium, fostering new growth and sustaining ecosystems. What seems like destruction often gives rise to fertility and rebirth. Volcanic ash, though initially destructive, can also create some of the most fertile soils on Earth, supporting agriculture for generations.\n\n\n");
                              // }







  }

//   UART_LCR = 0x83;
//   __asm__ volatile("fence iorw, iorw");

//   int DLL1 = pulp_read32(0x40000000)
//     , DLM1 = pulp_read32(0x40000004)
//     , LCR1 = UART_LCR;

//   UART_LCR = 0x03;
//   __asm__ volatile("fence iorw, iorw");



//   for (int i = 0; i < 140; i++) {
//     putchar('A');
    
//   }

//   for (int i = 0; i < 40; i++) {
//     printf("DLL = %02x, DLM = %02x, LCR = %02x\r\n", DLL1 , DLM1 , LCR1);
    
//   }

//     UART_LCR = 0x83;
//   __asm__ volatile("fence iorw, iorw");

//   DLL1 = pulp_read32(0x40000000)
//     , DLM1 = pulp_read32(0x40000004)
//     , LCR1 = UART_LCR;

//   UART_LCR = 0x03;
//   __asm__ volatile("fence iorw, iorw");


//  for (int i = 0; i < 30; i++) {
//     printf("-");
    
//   }

//   for (int i = 0; i < 40; i++) {
//     printf("DLL = %02x, DLM = %02x, LCR = %02x\r\n", DLL1 , DLM1 , LCR1);
    
//   }

//   uart_wait_tx_done();
   unlock(cid);
  return 0;
}