// SPDX-License-Identifier: Apache-2.0
/* Replace the factory general debug handler with its read-only operation.
 * The outer v19 dispatcher preserves the original reply framing.
 */
typedef unsigned int u32;
u32 debug_read(u32 *request, u32 *result)
{
	u32 address = request[2];

	if (request[0] || (address & 3) || address < 0x30000000U ||
	    address >= 0x70000000U)
		return 0;
	result[0] = *(volatile u32 *)address;
	return 1;
}
