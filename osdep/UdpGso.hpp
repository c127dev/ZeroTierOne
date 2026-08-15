/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * (c) ZeroTier, Inc.
 * https://www.zerotier.com/
 */

#ifndef ZT_UDPGSO_HPP
#define ZT_UDPGSO_HPP

#include <sys/socket.h>

namespace ZeroTier {

/**
 * Coalescing UDP transmit path using Linux UDP_SEGMENT (GSO).
 *
 * Phy::udpSend() issues one sendto() per packet. On a slow CPU that is not the
 * syscall boundary that hurts (getpid costs 0.12us against 3.09us for a 1400
 * byte sendto) but the per-packet trip through the UDP/IP stack, which is why
 * sendmmsg() measures no faster. UDP_SEGMENT hands the kernel one large buffer
 * plus a segment size, so the stack is walked once and the buffer is split
 * late. Measured on an Athlon II P360: 3.09us/pkt -> 0.57us/pkt.
 *
 * The kernel requires every segment to be the same size, except the last which
 * may be shorter. A batch is therefore flushed whenever the destination, the
 * socket, or the packet size changes.
 *
 * Batching is only armed on threads that promise to call flush() when they run
 * out of work - in practice the tap reader threads, whose read() loop drains
 * the tap and then blocks in select(). That boundary is the flush point, so a
 * packet is never delayed past the burst that produced it and a lone packet
 * (ping, DNS, a HELLO) is sent immediately. Threads that have not armed
 * batching are handed straight back to the caller for a normal sendto().
 */
class UdpGso {
  public:
	/**
	 * Globally enable or disable use of UDP_SEGMENT.
	 *
	 * Disabled by default; OneService turns it on from local.conf. If the
	 * kernel rejects a segmented send this is cleared automatically and the
	 * process falls back to per-packet sends for good.
	 */
	static void setEnabled(bool e);
	static bool enabled();

	/**
	 * Arm batching on the calling thread.
	 *
	 * The caller is promising to call flush() whenever it has no more packets
	 * to hand over. Threads that do not call this are never batched.
	 */
	static void armThread();

	/**
	 * Offer a packet for coalescing.
	 *
	 * @return true if the packet was consumed (queued or sent); false if the
	 *		 caller should send it itself, which is the case when batching is
	 *		 disabled, not armed on this thread, or not applicable
	 */
	static bool offer(int sock, const struct sockaddr* dest, const void* data, unsigned int len);

	/**
	 * Send this thread's pending batch, if any. Safe to call when empty.
	 */
	static void flush();
};

}	// namespace ZeroTier

#endif
