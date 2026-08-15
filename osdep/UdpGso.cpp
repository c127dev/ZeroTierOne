/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * (c) ZeroTier, Inc.
 * https://www.zerotier.com/
 */

#include "UdpGso.hpp"

#include "../node/Metrics.hpp"

#include <atomic>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Note: __LINUX__ is ZeroTier's own macro and comes from node/Constants.hpp,
// which this file does not include. Use the compiler's __linux__ so the real
// implementation is not silently replaced by the stub below.
#ifdef __linux__

#include <netinet/in.h>
#include <netinet/udp.h>
#include <sys/socket.h>

#ifndef UDP_SEGMENT
#define UDP_SEGMENT 103
#endif

namespace ZeroTier {

namespace {

	// Kernel caps a GSO write at UDP_MAX_SEGMENTS (64) segments and, being a
	// single UDP datagram before segmentation, at 65507 bytes of payload.
	static const unsigned int ZT_GSO_MAX_SEGMENTS = 64;
	static const unsigned int ZT_GSO_MAX_BYTES = 65507;

	// Nothing larger than this is worth coalescing; ZT_DEFAULT_PHYSMTU is 1432.
	static const unsigned int ZT_GSO_MAX_SEGSIZE = 1500;

	std::atomic<bool> s_enabled(false);

	// Prototype instrumentation. Batching can silently do nothing either
	// because it is not wired to the sending thread or because no backlog ever
	// forms, and those look identical from outside. Counting tells them apart.
	std::atomic<unsigned long> s_calls(0);		 // every udpSend that reached us
	std::atomic<unsigned long> s_notArmed(0);	 // ...from a thread that never armed
	std::atomic<unsigned long> s_offers(0);
	std::atomic<unsigned long> s_batches(0);
	std::atomic<unsigned long> s_segments(0);
	std::atomic<unsigned long> s_lastReport(0);

	// Off unless ZT_UDPGSO_DEBUG is set. Batching that silently does nothing
	// looks exactly like batching that is not wired up, so the counters are
	// worth keeping - just not in the journal by default.
	bool debugOn()
	{
		static const bool on = (getenv("ZT_UDPGSO_DEBUG") != (const char*)0);
		return on;
	}

	void maybeReport()
	{
		if (! debugOn()) {
			return;
		}
		const unsigned long c = s_calls.load();
		if ((c - s_lastReport.load()) < 50000) {
			return;
		}
		s_lastReport.store(c);
		const unsigned long b = s_batches.load();
		fprintf(
			stderr,
			"UdpGso: calls=%lu notArmed=%lu offers=%lu batches=%lu segments=%lu avg_segments_per_batch=%.2f\n",
			c,
			s_notArmed.load(),
			s_offers.load(),
			b,
			s_segments.load(),
			b ? ((double)s_segments.load() / (double)b) : 0.0);
	}

	struct Batch {
		int sock;
		struct sockaddr_storage dest;
		socklen_t destLen;
		unsigned int segSize;	// size of every segment except possibly the last
		unsigned int count;
		unsigned int len;	// bytes used in buf
		unsigned char buf[ZT_GSO_MAX_BYTES];

		Batch() : sock(-1), destLen(0), segSize(0), count(0), len(0)
		{
			memset(&dest, 0, sizeof(dest));
		}
	};

	thread_local bool t_armed = false;
	thread_local Batch* t_batch = (Batch*)0;

	inline socklen_t sockaddrLen(const struct sockaddr* sa)
	{
		return (sa->sa_family == AF_INET6) ? (socklen_t)sizeof(struct sockaddr_in6) : (socklen_t)sizeof(struct sockaddr_in);
	}

	inline bool sameDest(const struct sockaddr_storage& a, const struct sockaddr* b, socklen_t blen)
	{
		return (((const struct sockaddr*)&a)->sa_family == b->sa_family) && (memcmp(&a, b, (size_t)blen) == 0);
	}

	// One plain sendto, used for single-segment batches and as the fallback
	// when the kernel refuses a segmented write.
	inline bool sendPlain(int sock, const struct sockaddr* dest, socklen_t destLen, const void* data, unsigned int len)
	{
		const bool ok = ((long)::sendto(sock, data, len, 0, dest, destLen) == (long)len);
		if (ok) {
			Metrics::udp_send += len;
		}
		return ok;
	}

	void sendBatch(Batch& b)
	{
		if (b.count == 0) {
			return;
		}

		s_batches.fetch_add(1);
		s_segments.fetch_add(b.count);

		const unsigned int len = b.len;
		const unsigned int count = b.count;
		const unsigned int segSize = b.segSize;
		const int sock = b.sock;
		struct sockaddr_storage dest = b.dest;
		const socklen_t destLen = b.destLen;

		// Reset before sending so a failure path cannot re-enter with stale state.
		b.count = 0;
		b.len = 0;
		b.segSize = 0;
		b.sock = -1;

		if (count == 1) {
			sendPlain(sock, (const struct sockaddr*)&dest, destLen, b.buf, len);
			return;
		}

		struct msghdr mh;
		struct iovec iv;
		unsigned char cbuf[CMSG_SPACE(sizeof(uint16_t))];

		memset(&mh, 0, sizeof(mh));
		memset(cbuf, 0, sizeof(cbuf));
		iv.iov_base = b.buf;
		iv.iov_len = len;
		mh.msg_iov = &iv;
		mh.msg_iovlen = 1;
		mh.msg_name = (void*)&dest;
		mh.msg_namelen = destLen;
		mh.msg_control = cbuf;
		mh.msg_controllen = sizeof(cbuf);

		struct cmsghdr* cm = CMSG_FIRSTHDR(&mh);
		cm->cmsg_level = SOL_UDP;
		cm->cmsg_type = UDP_SEGMENT;
		cm->cmsg_len = CMSG_LEN(sizeof(uint16_t));
		*((uint16_t*)CMSG_DATA(cm)) = (uint16_t)segSize;

		const long n = (long)::sendmsg(sock, &mh, 0);
		if (n == (long)len) {
			Metrics::udp_send += len;
			return;
		}

		// EIO / ENOPROTOOPT / EINVAL means this kernel or socket will not do
		// GSO. Stop trying and unpick the batch by hand so nothing is lost.
		if (n < 0) {
			fprintf(stderr, "UdpGso: sendmsg failed: %s (count=%u segSize=%u len=%u)\n", strerror(errno), count, segSize, len);
		}
		if ((n < 0) && ((errno == EIO) || (errno == ENOPROTOOPT) || (errno == EINVAL) || (errno == EOPNOTSUPP))) {
			fprintf(stderr, "UdpGso: disabling UDP_SEGMENT for the rest of this run\n");
			s_enabled.store(false);
		}
		if (n < 0) {
			unsigned int off = 0;
			while (off < len) {
				const unsigned int thisLen = ((len - off) < segSize) ? (len - off) : segSize;
				sendPlain(sock, (const struct sockaddr*)&dest, destLen, b.buf + off, thisLen);
				off += thisLen;
			}
		}
	}

}	// anonymous namespace

void UdpGso::setEnabled(bool e)
{
	s_enabled.store(e);
}

bool UdpGso::enabled()
{
	return s_enabled.load();
}

void UdpGso::armThread()
{
	t_armed = true;
	if (debugOn()) {
		fprintf(stderr, "UdpGso: batching armed on this thread (enabled=%d)\n", (int)s_enabled.load());
	}
}

void UdpGso::flush()
{
	if (t_batch) {
		sendBatch(*t_batch);
	}
}

bool UdpGso::offer(int sock, const struct sockaddr* dest, const void* data, unsigned int len)
{
	s_calls.fetch_add(1);
	maybeReport();
	if (! s_enabled.load()) {
		return false;
	}
	if (! t_armed) {
		s_notArmed.fetch_add(1);
		return false;
	}
	if ((len == 0) || (len > ZT_GSO_MAX_SEGSIZE)) {
		// Oversized packets are not worth coalescing, but anything already
		// queued must go out first or it would overtake this one.
		flush();
		return false;
	}
	if ((dest->sa_family != AF_INET) && (dest->sa_family != AF_INET6)) {
		flush();
		return false;
	}

	s_offers.fetch_add(1);

	if (! t_batch) {
		t_batch = new Batch();
	}
	Batch& b = *t_batch;
	const socklen_t dlen = sockaddrLen(dest);

	// A batch is one socket, one destination, one segment size. Anything that
	// breaks that closes the current batch and starts a new one.
	if (b.count > 0) {
		if ((b.sock != sock) || (! sameDest(b.dest, dest, dlen)) || (len > b.segSize)) {
			sendBatch(b);
		}
	}

	if (b.count == 0) {
		b.sock = sock;
		memcpy(&b.dest, dest, (size_t)dlen);
		b.destLen = dlen;
		b.segSize = len;
	}

	memcpy(b.buf + b.len, data, len);
	b.len += len;
	++b.count;

	// A short packet can only ever be the final segment, so close here. Also
	// close on the kernel's segment and byte ceilings.
	if ((len < b.segSize) || (b.count >= ZT_GSO_MAX_SEGMENTS) || ((b.len + b.segSize) > ZT_GSO_MAX_BYTES)) {
		sendBatch(b);
	}

	return true;
}

}	// namespace ZeroTier

#else	 // !__linux__

namespace ZeroTier {
void UdpGso::setEnabled(bool)
{
}
bool UdpGso::enabled()
{
	return false;
}
void UdpGso::armThread()
{
}
void UdpGso::flush()
{
}
bool UdpGso::offer(int, const struct sockaddr*, const void*, unsigned int)
{
	return false;
}
}	// namespace ZeroTier

#endif
