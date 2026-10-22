// SPDX-License-Identifier: Apache-2.0
/* Radio group histogram0..7,8+;sum;four stop reasons;max;reserved. */
typedef unsigned int u32;
__attribute__((section(".trace_data")))
volatile u32 radio_trace[16] = { 0 };
void trace_aggregate(u32 count)
{
 radio_trace[count < 8 ? count : 8]++;
 radio_trace[9] += count;
 if (count > radio_trace[14]) radio_trace[14] = count;
}
