/*
 * WiVRn VR streaming
 * Copyright (C) 2022  Guillaume Meunier <guillaume.meunier@centraliens.net>
 * Copyright (C) 2022  Patrick Nicolas <patricknicolas@laposte.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "wivrn_sockets.h"

#include <arpa/inet.h>
#include <cassert>
#include <memory>
#include <netdb.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <system_error>
#include <unistd.h>

const char * wivrn::invalid_packet::what() const noexcept
{
	return "Invalid packet";
}

const char * wivrn::socket_shutdown::what() const noexcept
{
	return "Socket shutdown";
}

wivrn::fd_base::fd_base(wivrn::fd_base && other) :
        fd(other.fd)
{
	other.fd = -1;
}

wivrn::fd_base & wivrn::fd_base::operator=(wivrn::fd_base && other)
{
	std::swap(fd, other.fd);
	return *this;
}

wivrn::fd_base::~fd_base()
{
	if (fd >= 0)
		::close(fd);
}

wivrn::UDP::UDP(const bool ipv4)
{
	fd = socket(ipv4 ? AF_INET : AF_INET6, SOCK_DGRAM, 0);

	if (fd < 0)
		throw std::system_error{errno, std::generic_category()};
}

wivrn::UDP::UDP(int fd)
{
	this->fd = fd;
}

void wivrn::UDP::bind(int port)
{
	sockaddr_in6 bind_addr{};
	bind_addr.sin6_family = AF_INET6;
	bind_addr.sin6_addr = IN6ADDR_ANY_INIT;
	bind_addr.sin6_port = htons(port);

	if (::bind(fd, (sockaddr *)&bind_addr, sizeof(bind_addr)) < 0)
		throw std::system_error{errno, std::generic_category()};
}

void wivrn::UDP::connect(in6_addr address, int port)
{
	sockaddr_in6 sa = {};
	sa.sin6_family = AF_INET6;
	sa.sin6_addr = address;
	sa.sin6_port = htons(port);

	if (::connect(fd, (sockaddr *)&sa, sizeof(sa)) < 0)
		throw std::system_error{errno, std::generic_category()};
}

void wivrn::UDP::connect(in_addr address, int port)
{
	sockaddr_in sa = {};
	sa.sin_family = AF_INET;
	sa.sin_addr = address;
	sa.sin_port = htons(port);

	if (::connect(fd, (sockaddr *)&sa, sizeof(sa)) < 0)
		throw std::system_error{errno, std::generic_category()};
}

void wivrn::UDP::subscribe_multicast(in6_addr address)
{
	assert(IN6_IS_ADDR_MULTICAST(&address));

	ipv6_mreq subscribe{};
	subscribe.ipv6mr_multiaddr = address;

	if (setsockopt(fd, IPPROTO_IPV6, IPV6_JOIN_GROUP, &subscribe, sizeof(subscribe)) < 0)
	{
		::close(fd);
		throw std::system_error{errno, std::generic_category()};
	}
}

void wivrn::UDP::unsubscribe_multicast(in6_addr address)
{
	assert(IN6_IS_ADDR_MULTICAST(&address));

	ipv6_mreq subscribe{};
	subscribe.ipv6mr_multiaddr = address;

	if (setsockopt(fd, IPPROTO_IPV6, IPV6_LEAVE_GROUP, &subscribe, sizeof(subscribe)) < 0)
	{
		::close(fd);
		throw std::system_error{errno, std::generic_category()};
	}
}

void wivrn::UDP::set_receive_buffer_size(int size)
{
	setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
}

void wivrn::UDP::set_send_buffer_size(int size)
{
	setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size));
}

void wivrn::UDP::set_tos(int tos)
{
	int err = setsockopt(fd, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));
	if (err == -1)
	{
		throw std::system_error{errno, std::generic_category()};
	}
}

void wivrn::TCP::init()
{
	int nodelay = 1;
	if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay)) < 0)
	{
		::close(fd);
		throw std::system_error{errno, std::generic_category()};
	}

	#ifndef MSG_NOSIGNAL
	int nosigpipe = 1;
	if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &nosigpipe, sizeof(nosigpipe)) < 0)
	{
		::close(fd);
		throw std::system_error{errno, std::generic_category()};
	}
	#endif

	mutex = std::make_unique<std::mutex>();
}

wivrn::TCP::TCP(int fd)
{
	this->fd = fd;

	init();
}

wivrn::TCP::TCP(in6_addr address, int port)
{
	fd = socket(AF_INET6, SOCK_STREAM, 0);

	if (fd < 0)
		throw std::system_error{errno, std::generic_category()};

	sockaddr_in6 sa;
	sa.sin6_family = AF_INET6;
	sa.sin6_addr = address;
	sa.sin6_port = htons(port);

	if (connect(fd, (sockaddr *)&sa, sizeof(sa)) < 0)
	{
		::close(fd);
		throw std::system_error{errno, std::generic_category()};
	}

	init();
}

wivrn::TCP::TCP(in_addr address, int port)
{
	fd = socket(AF_INET, SOCK_STREAM, 0);

	if (fd < 0)
		throw std::system_error{errno, std::generic_category()};

	sockaddr_in sa;
	sa.sin_family = AF_INET;
	sa.sin_addr = address;
	sa.sin_port = htons(port);

	if (connect(fd, (sockaddr *)&sa, sizeof(sa)) < 0)
	{
		::close(fd);
		throw std::system_error{errno, std::generic_category()};
	}

	init();
}

wivrn::TCPListener::TCPListener()
{
}

wivrn::TCPListener::TCPListener(int port)
{
	fd = socket(AF_INET6, SOCK_STREAM, 0);

	if (fd < 0)
	{
		throw std::system_error{errno, std::generic_category()};
	}

	int reuse_addr = 1;
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse_addr, sizeof(reuse_addr)) < 0)
	{
		::close(fd);
		throw std::system_error{errno, std::generic_category()};
	}

	sockaddr_in6 addr{};
	addr.sin6_family = AF_INET6;
	addr.sin6_port = htons(port);
	addr.sin6_addr = in6addr_any;

	if (bind(fd, (sockaddr *)&addr, sizeof(addr)) < 0)
	{
		::close(fd);
		throw std::system_error{errno, std::generic_category()};
	}

	int backlog = 1;
	if (listen(fd, backlog) < 0)
	{
		::close(fd);
		throw std::system_error{errno, std::generic_category()};
	}
}

std::pair<wivrn::deserialization_packet, sockaddr_in6> wivrn::UDP::receive_from_raw()
{
	sockaddr_in6 addr;
	socklen_t addrlen = sizeof(addr);

	size_t size = recvfrom(fd, nullptr, 0, MSG_PEEK | MSG_TRUNC, (sockaddr *)&addr, &addrlen);

#if defined(__cpp_lib_smart_ptr_for_overwrite) && __cpp_lib_smart_ptr_for_overwrite >= 202002L
	auto buffer = std::make_shared_for_overwrite<uint8_t[]>(size);
#else
	std::shared_ptr<uint8_t[]> buffer(new uint8_t[size]);
#endif
	ssize_t received = recvfrom(fd, buffer.get(), size, 0, (sockaddr *)&addr, &addrlen);
	if (received < 0)
		throw std::system_error{errno, std::generic_category()};

	bytes_received_ += received;

	auto tmp = buffer.get();
	return {deserialization_packet{std::move(buffer), std::span(tmp, received)}, addr};
}

wivrn::deserialization_packet wivrn::UDP::receive_pending()
{
	if (messages.empty())
		return {};

	auto span = messages.back();
	messages.pop_back();
	return deserialization_packet{buffer, span};
}

#ifdef __APPLE__
#include <sys/syscall.h>

struct mmsghdr {
	struct msghdr msg_hdr;
	size_t msg_len;
};
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
static int sendmmsg(const int fd, struct mmsghdr *const vmessages, const unsigned int vlen, const int flags) {
	int result;
	do {
		result = syscall(SYS_sendmsg_x, fd, vmessages, vlen, flags);
	} while(result < 0 && errno == EINTR);
	return result;
}
static int recvmmsg(const int fd, struct mmsghdr *const vmessages, const unsigned int vlen, const int flags, struct timespec*) {
	int result;
	do {
		result = syscall(SYS_recvmsg_x, fd, vmessages, vlen, flags);
	} while(result < 0 && errno == EINTR);
	return result;
}
#pragma GCC diagnostic pop
#endif

wivrn::deserialization_packet wivrn::UDP::receive_raw()
{
	if (not messages.empty())
	{
		auto span = messages.back();
		messages.pop_back();
		return deserialization_packet{buffer, span};
	}

	static const size_t message_size = 2048;
	static const size_t num_messages = 20;
#if defined(__cpp_lib_smart_ptr_for_overwrite) && __cpp_lib_smart_ptr_for_overwrite >= 202002L
	buffer = std::make_shared_for_overwrite<uint8_t[]>(message_size * num_messages);
#else
	buffer.reset(new uint8_t[message_size * num_messages]);
#endif
	std::vector<iovec> iovecs;
	std::vector<mmsghdr> mmsgs;
	iovecs.reserve(num_messages);
	mmsgs.reserve(num_messages);
	for (size_t i = 0; i < num_messages; ++i)
	{
		iovecs.push_back({
		        .iov_base = buffer.get() + message_size * i,
		        .iov_len = message_size,
		});
		mmsgs.push_back(
		        {
		                .msg_hdr = {
		                        .msg_iov = &iovecs.back(),
		                        .msg_iovlen = 1,
		                },
		        });
	}

	int received = recvmmsg(fd, mmsgs.data(), num_messages, MSG_DONTWAIT, nullptr);

	if (received < 0)
		throw std::system_error{errno, std::generic_category()};
	if (received == 0)
		throw socket_shutdown();

	messages.reserve(received);
	for (int i = received - 1; i > 0; --i)
	{
		messages.emplace_back(
		        (uint8_t *)iovecs[i].iov_base,
		        mmsgs[i].msg_len);
		bytes_received_ += mmsgs[i].msg_len;
	}

	return deserialization_packet{buffer, std::span(buffer.get(), mmsgs[0].msg_len)};
}

void wivrn::UDP::send_raw(const std::vector<uint8_t> & data)
{
	ssize_t sent = ::send(fd, data.data(), data.size(), 0);
	if (sent < 0)
		throw std::system_error{errno, std::generic_category()};

	bytes_sent_ += sent;
}

void wivrn::UDP::send_raw(const std::vector<std::span<uint8_t>> & data)
{
	thread_local std::vector<iovec> spans;
	spans.clear();
	for (const auto & span: data)
		spans.push_back({(void *)span.data(), span.size()});

	if (::writev(fd, spans.data(), spans.size()) < 0)
		throw std::system_error{errno, std::generic_category()};
}

void wivrn::UDP::send_many_raw(std::span<const std::vector<std::span<uint8_t>> *> data)
{
	thread_local std::vector<iovec> iovecs;
	thread_local std::vector<mmsghdr> mmsgs;
	iovecs.clear();
	mmsgs.clear();
	for (const auto & message: data)
	{
		for (const auto & span: *message)
		{
			iovecs.push_back(
			        {
			                .iov_base = span.data(),
			                .iov_len = span.size_bytes(),
			        });
		}
	}
	size_t i = 0;
	for (const auto & message: data)
	{
		mmsgs.push_back(
		        {
		                .msg_hdr = {
		                        .msg_iov = &iovecs[i],
		                        .msg_iovlen = static_cast<decltype(msghdr::msg_iovlen)>(message->size()),
		                },
		        });
		i += message->size();
	}
	// sendmmsg may not send all messages, just consider them as lost for UDP
	if (sendmmsg(fd, mmsgs.data(), mmsgs.size(), 0) < 0)
		throw std::system_error{errno, std::generic_category()};
}

wivrn::deserialization_packet wivrn::TCP::receive_raw()
{
	ssize_t expected_size;

	if (data.size_bytes() < sizeof(uint16_t))
	{
		expected_size = sizeof(uint16_t) - data.size_bytes();
	}
	else
	{
		uint32_t payload_size = *reinterpret_cast<uint16_t *>(data.data());
		expected_size = payload_size + sizeof(uint16_t) - data.size_bytes();
	}

	if (expected_size > capacity_left)
	{
		size_t new_size = std::max<size_t>(data.size_bytes() + expected_size,
		                                   4096);
		auto old = std::move(buffer);
#if defined(__cpp_lib_smart_ptr_for_overwrite) && __cpp_lib_smart_ptr_for_overwrite >= 202002L
		buffer = std::make_shared_for_overwrite<uint8_t[]>(new_size);
#else
		buffer.reset(new uint8_t[new_size]);
#endif
		memcpy(buffer.get(), data.data(), data.size_bytes());
		data = std::span(buffer.get(), data.size());
		capacity_left = new_size - data.size_bytes();
	}

	ssize_t received = recv(fd, &*data.end(), capacity_left, MSG_DONTWAIT);

	if (received < 0)
		throw std::system_error{errno, std::generic_category()};

	if (received == 0)
		throw socket_shutdown{};

	bytes_received_ += received;

	data = std::span(data.data(), data.size() + received);
	capacity_left -= received;
	if (data.size_bytes() < sizeof(uint16_t))
		return {};

	uint32_t payload_size = *reinterpret_cast<uint16_t *>(data.data());
	if (payload_size == 0)
		throw std::runtime_error("Invalid packet: 0 size");

	if (data.size_bytes() < sizeof(uint16_t) + payload_size)
		return {};

	auto span = data.subspan(sizeof(uint16_t), payload_size);
	data = data.subspan(sizeof(uint16_t) + payload_size);
	return deserialization_packet{buffer, span};
}

wivrn::deserialization_packet wivrn::TCP::receive_pending()
{
	if (data.size_bytes() < sizeof(uint16_t))
		return {};

	uint32_t payload_size = *reinterpret_cast<uint16_t *>(data.data());
	if (payload_size == 0)
		throw std::runtime_error("Invalid packet: 0 size");

	if (data.size_bytes() < sizeof(uint16_t) + payload_size)
		return {};

	auto span = data.subspan(sizeof(uint16_t), payload_size);
	data = data.subspan(sizeof(uint16_t) + payload_size);
	return deserialization_packet{buffer, span};
}

void wivrn::TCP::send_raw(const std::vector<std::span<uint8_t>> & spans)
{
	thread_local std::vector<iovec> iovecs;
	iovecs.clear();

	uint16_t size = 0;
	iovecs.push_back({&size, sizeof(size)});
	for (const auto & span: spans)
	{
		size += span.size_bytes();
		iovecs.push_back({span.data(), span.size_bytes()});
	}

	msghdr hdr{
	        .msg_name = nullptr,
	        .msg_namelen = 0,
	        .msg_iov = iovecs.data(),
	        .msg_iovlen = static_cast<decltype(msghdr::msg_iovlen)>(iovecs.size()),
	        .msg_control = nullptr,
	        .msg_controllen = 0,
	        .msg_flags = 0,
	};

	std::lock_guard lock(*mutex);
	while (true)
	{
		const auto & data = spans[0];
		#ifdef MSG_NOSIGNAL
		ssize_t sent = ::sendmsg(fd, &hdr, MSG_NOSIGNAL);
		#else
		ssize_t sent = ::sendmsg(fd, &hdr, 0);
		#endif

		if (sent == 0)
			throw socket_shutdown{};

		if (sent < 0)
			throw std::system_error{errno, std::generic_category()};

		bytes_sent_ += sent;

		// iov fully consumed
		while (hdr.msg_iovlen > 0 and sent >= hdr.msg_iov[0].iov_len)
		{
			sent -= hdr.msg_iov[0].iov_len;
			++hdr.msg_iov;
			--hdr.msg_iovlen;
		}
		if (hdr.msg_iovlen == 0)
			return;
		hdr.msg_iov[0].iov_base = (void *)((uintptr_t)hdr.msg_iov[0].iov_base + sent);
		hdr.msg_iov[0].iov_len -= sent;
	}
}

void wivrn::TCP::send_many_raw(std::span<const std::vector<std::span<uint8_t>> *> data)
{
	thread_local std::vector<iovec> iovecs;
	thread_local std::vector<uint16_t> sizes;
	iovecs.clear();
	sizes.clear();

	sizes.reserve(data.size());
	for (const auto & spans: data)
	{
		auto & size = sizes.emplace_back(0);
		iovecs.emplace_back(&size, sizeof(size));
		for (const auto & span: *spans)
		{
			size += span.size_bytes();
			iovecs.emplace_back(span.data(), span.size_bytes());
		}
	}

	msghdr hdr{
	        .msg_name = nullptr,
	        .msg_namelen = 0,
	        .msg_iov = iovecs.data(),
	        .msg_iovlen = static_cast<decltype(msghdr::msg_iovlen)>(iovecs.size()),
	        .msg_control = nullptr,
	        .msg_controllen = 0,
	        .msg_flags = 0,
	};

	std::lock_guard lock(*mutex);
	while (true)
	{
		ssize_t sent = ::sendmsg(fd, &hdr, MSG_NOSIGNAL);

		if (sent == 0)
			throw socket_shutdown{};

		if (sent < 0)
			throw std::system_error{errno, std::generic_category()};

		bytes_sent_ += sent;

		// iov fully consumed
		while (hdr.msg_iovlen > 0 and sent >= hdr.msg_iov[0].iov_len)
		{
			sent -= hdr.msg_iov[0].iov_len;
			++hdr.msg_iov;
			--hdr.msg_iovlen;
		}
		if (hdr.msg_iovlen == 0)
			return;
		hdr.msg_iov[0].iov_base = (void *)((uintptr_t)hdr.msg_iov[0].iov_base + sent);
		hdr.msg_iov[0].iov_len -= sent;
	}
}
