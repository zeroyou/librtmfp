/*
This file is a part of MonaSolutions Copyright 2017
mathieu.poux[a]gmail.com
jammetthomas[a]gmail.com

This program is free software: you can redistribute it and/or
modify it under the terms of the the Mozilla Public License v2.0.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
Mozilla Public License v. 2.0 received along this program for more
details (or else see http://mozilla.org/MPL/2.0/).

*/


#include "Base/Socket.h"
#if !defined(_WIN32)
#include <fcntl.h>
#endif


using namespace std;

namespace Base {

Socket::Socket(Type type) :
#if !defined(_WIN32)
	_pWeakThis(NULL), _firstWritable(true),
#endif
	externDecoder(false), _nonBlockingMode(false), _listening(false), _receiving(0), _queueing(0), _recvBufferSize(Net::GetRecvBufferSize()), _sendBufferSize(Net::GetSendBufferSize()), _reading(0), type(type), _recvTime(0), _sendTime(0), _id(NET_INVALID_SOCKET), _threadReceive(0) {

	init();
}

// private constructor used just by Socket::accept, TCP initialized and connected socket
Socket::Socket(NET_SOCKET id, const sockaddr& addr) : _peerAddress(addr), _address(IPAddress::Loopback(),0), // computable!
#if !defined(_WIN32)
	_pWeakThis(NULL), _firstWritable(true),
#endif
	externDecoder(false), _nonBlockingMode(false), _listening(false), _receiving(0), _queueing(0), _recvBufferSize(Net::GetRecvBufferSize()), _sendBufferSize(Net::GetSendBufferSize()), _reading(0), type(Socket::TYPE_STREAM), _recvTime(Time::Now()), _sendTime(0), _id(id), _threadReceive(0) {

	init();
}


Socket::~Socket() {
	if (externDecoder) {
		pDecoder->onRelease(self);
		delete pDecoder;
	}
	if (_id == NET_INVALID_SOCKET)
		return;
	// ::printf("DELETE socket %d\n", _id);
	// gracefull disconnection => flush + shutdown + close
	/// shutdown + flush
	Exception ignore;
	flush(ignore, true);
	close();
	NET_CLOSESOCKET(_id);
}

bool Socket::close(ShutdownType type) {
	if (::shutdown(_id, type) == 0)
		return true;
	Net::LastError(); // to pick up _errno
	return false;
}

bool Socket::shutdown(Socket::ShutdownType type) {
	if (_id == NET_INVALID_SOCKET)
		return false;
	// Try to flush before to stop sending
	if (type) { // type = SEND or BOTH
		Exception ignore;
		flush(ignore);
	}
	return close(type);
}

void Socket::init() {

	if (_id == NET_INVALID_SOCKET) {
		_id = ::socket(AF_INET6, type, 0);
		if (_id == NET_INVALID_SOCKET) {
			SetException(_ex, Net::LastError());
			return;
		}
	}

	Exception ignore;
	// to be compatible IPv6 and IPv4!
	setOption(ignore, IPPROTO_IPV6, IPV6_V6ONLY, 0);
	// Set Recv/Send Buffer size as configured in Net, and before any connect/bind!
	setOption(ignore, SOL_SOCKET, SO_RCVBUF, _recvBufferSize.load());
	setOption(ignore, SOL_SOCKET, SO_SNDBUF, _sendBufferSize.load());

	if (type==Socket::TYPE_STREAM)
		setNoDelay(ignore,true); // to avoid the nagle algorithm, ignore error if not possible
}

UInt32 Socket::available() const {
	UInt32 value;
#if defined(SO_NREAD)
	Exception ex;
	if (getOption(ex, SOL_SOCKET, SO_NREAD, value))
		return value;
#endif
#if defined(_WIN32)
	if (::ioctlsocket(_id, FIONREAD, (u_long*)&value))
		return 0;
	// size UDP packet on windows is impossible to determinate so take a multiple of 2 grater than max possible MTU (~1500 bytes)
	return type == TYPE_DATAGRAM && value>2048 ? 2048 : value;
#else
	return ::ioctl(_id, FIONREAD, &value) ? 0 : value;
#endif
}

bool Socket::setNonBlockingMode(Exception& ex, bool value) {
#if defined(_WIN32)
	if (!ioctlsocket(_id, FIONBIO, (u_long*)&value))
		return _nonBlockingMode = value;
#else
	int flags = fcntl(_id, F_GETFL, 0);
	if (flags != -1) {
		if (value)
			flags |= O_NONBLOCK;
		else
			flags &= ~O_NONBLOCK;
		if (fcntl(_id, F_SETFL, flags) != -1)
			return _nonBlockingMode = value;
	}
#endif
	SetException(ex, Net::LastError(), " (value=", value, ")");
	return false;
}

bool Socket::setRecvBufferSize(Exception& ex, int size) { 
	if (!setOption(ex, SOL_SOCKET, SO_RCVBUF, size))
		return false;
	_recvBufferSize = size;
	return true;
}
bool Socket::setSendBufferSize(Exception& ex, int size) {
	if (!setOption(ex, SOL_SOCKET, SO_SNDBUF, size))
		return false;
	_sendBufferSize = size;
	return true;
}

const SocketAddress& Socket::address() const {
	if (_address && !_address.port()) {
		// computable!
		union {
			struct sockaddr_in  sa_in;
			struct sockaddr_in6 sa_in6;
		} addr;
		NET_SOCKLEN addrSize = sizeof(addr);
		if (::getsockname(_id, (sockaddr*)&addr, &addrSize) == 0)
			_address.set((sockaddr&)addr);
	}
	return _address;
}


bool Socket::setLinger(Exception& ex,bool on, int seconds) {
	if (_ex) {
		ex = _ex;
		return false;
	}
	struct linger l;
	l.l_onoff  = on ? 1 : 0;
	l.l_linger = seconds;
	return setOption(ex, SOL_SOCKET, SO_LINGER, l);
}
bool Socket::getLinger(Exception& ex, bool& on, int& seconds) const {
	if (_ex) {
		ex = _ex;
		return false;
	}
	struct linger l;
	if (!getOption(ex, SOL_SOCKET, SO_LINGER, l))
		return false;
	seconds = l.l_linger;
	on = l.l_onoff != 0;
	return true;
}
	
void Socket::setReusePort(bool value) {
#ifdef SO_REUSEPORT
	Exception ex; // ignore error, since not all implementations support SO_REUSEPORT, even if the macro is defined
	setOption(ex,SOL_SOCKET, SO_REUSEPORT, value ? 1 : 0);
#endif
}
bool Socket::getReusePort() const {
#ifdef SO_REUSEPORT
	Exception ex;
	bool value;
	if(getOption(ex,SOL_SOCKET, SO_REUSEPORT,value))
		return value;;
#endif
	return false;
}

bool Socket::joinGroup(Exception& ex, const IPAddress& ip, UInt32 interfaceIndex) {
	if (ip.family() == IPAddress::IPv4) {
		struct ip_mreq mreq;
		memcpy(&mreq.imr_multiaddr, ip.data(), ip.size());
		mreq.imr_interface.s_addr = htonl(interfaceIndex);
		return setOption(ex, IPPROTO_IP, IP_ADD_MEMBERSHIP, mreq);
	}
	struct ipv6_mreq mreq;
	memcpy(&mreq.ipv6mr_multiaddr, ip.data(), ip.size());
	mreq.ipv6mr_interface = interfaceIndex;
	return setOption(ex, IPPROTO_IPV6, IPV6_ADD_MEMBERSHIP, mreq);
}

void Socket::leaveGroup(const IPAddress& ip, UInt32 interfaceIndex) {
	Exception ex;
	if (ip.family() == IPAddress::IPv4) {
		struct ip_mreq mreq;
		memcpy(&mreq.imr_multiaddr, ip.data(), ip.size());
		mreq.imr_interface.s_addr = htonl(interfaceIndex);
		setOption(ex, IPPROTO_IP, IP_DROP_MEMBERSHIP, mreq);
	} else {
		struct ipv6_mreq mreq;
		memcpy(&mreq.ipv6mr_multiaddr, ip.data(), ip.size());
		mreq.ipv6mr_interface = interfaceIndex;
		setOption(ex, IPPROTO_IPV6, IPV6_DROP_MEMBERSHIP, mreq);
	}
}

bool Socket::accept(Exception& ex, shared<Socket>& pSocket) {
	if (_ex) {
		ex = _ex;
		return false;
	}
	union {
		struct sockaddr_in  sa_in;
		struct sockaddr_in6 sa_in6;
	} addr;
	NET_SOCKLEN addrSize = sizeof(addr);
	NET_SOCKET sockfd;
	int error;
	do {
		sockfd = ::accept(_id, (sockaddr*)&addr, &addrSize);
	} while (sockfd == NET_INVALID_SOCKET && (error = Net::LastError()) == NET_EINTR);
	if (sockfd == NET_INVALID_SOCKET) {
		if (error == NET_EAGAIN)
			error = NET_EWOULDBLOCK;
		SetException(ex, error);
		return false;
	}
	pSocket.reset(newSocket(ex, sockfd, (sockaddr&)addr));
	if (pSocket)
		return true;
	NET_CLOSESOCKET(sockfd);
	return false;
}

bool Socket::connect(Exception& ex, const SocketAddress& address, UInt16 timeout) {
	if (_ex) {
		ex = _ex;
		return false;
	}

	bool block = false;
	if (timeout && !_nonBlockingMode) {
		block = true;
		if (!setNonBlockingMode(ex, true))
			return false;
	}

	// Allow to call multiple time this method, it can help on windows target to etablish a connection instead of waiting the connection!
	int rc;
	if (type == Socket::TYPE_DATAGRAM && !address) { // fix a UDP disconnect problem (Wildcard IPv4-IPv6 mapping != sockaddr null!)
		if (!_peerAddress)
			return true; // no change, usefull to fix double disconnection issue (can not assign 0.0.0.0 address!)
		rc = ::connect(_id, SocketAddress::Wildcard(IPAddress::IPv6).data(), SocketAddress::Wildcard(IPAddress::IPv6).size());
	} else
		rc = ::connect(_id, address.data(), address.size());

	if (block)
		setNonBlockingMode(ex, false); // reset blocking mode (as no effect if was subscribed to SocketEngine which forces non-blocking mode)
	
	int error(0);
	if (rc) {
		error = Net::LastError();
		if (_peerAddress || error == NET_EISCONN) { // if already connected (_peerAddress is true OR error ISCONN)
			if (_peerAddress == address)
				return true; // already connected to this address => no error
			SetException(ex, NET_EISCONN, " (address=", address, ")");
			return false;  // already connected to one other address => error
		}
		/// EINPROGRESS/NET_EWOULDBLOCK => first call to connect
		/// EALREADY => second call to connect
		if (error != NET_EWOULDBLOCK && error != NET_EALREADY && error != NET_EINPROGRESS) {
			SetException(ex, error, " (address=", address, ")");
			return false; // fail
		}

		error = NET_EWOULDBLOCK;

		if (timeout) {
			fd_set fdset;
			timeval tv;
			FD_ZERO(&fdset);
			FD_SET(_id, &fdset);
			tv.tv_sec = timeout;
			tv.tv_usec = 0;

			rc = select(_id + 1, NULL, &fdset, NULL, &tv);
			if (rc <= 0) {
				// timeout (=> connection refused) OR Error!
				SetException(ex, rc ? NET_ECONNREFUSED : Net::LastError(), " (address=", address, ")");
				return false;
			}
			// connected!
		} else
			SetException(ex, error, " (address=", address, ")");
	}

	_address.set(IPAddress::Loopback(), 0); // to advise that address must be computed
	_peerAddress = address;
	return error ? false : true;
}


bool Socket::bind(Exception& ex, const SocketAddress& addr) {
	if (_ex) {
		ex = _ex;
		return false;
	}

	SocketAddress address(addr);
	if (type == TYPE_STREAM) {
		// http://stackoverflow.com/questions/3757289/tcp-option-so-linger-zero-when-its-required =>
		/// If you must restart your server application which currently has thousands of client connections you might consider
		// setting this socket option to avoid thousands of server sockets in TIME_WAIT(when calling close() from the server end)
		// as this might prevent the server from getting available ports for new client connections after being restarted.
		setLinger(ex, true, 0); // avoid a TCP port overriding on server usage
		setReuseAddress(ex, true); // to avoid a conflict between address bind on not exactly same address (see http://stackoverflow.com/questions/14388706/socket-options-so-reuseaddr-and-so-reuseport-how-do-they-differ-do-they-mean-t)
	} else if (address.host().isMulticast()) {
		// if address is multicast then connect to the ip group multicast!
		joinGroup(ex, address.host());
		address.host().set(IPAddress::Wildcard());
	}
	if (::bind(_id, address.data(), address.size()) != 0) {
		SetException(ex, Net::LastError(), " (address=", address, ")");
		return false;
	}
	if (address)
		_address = address; // if port = 0, will be computed!
	else
		_address.set(IPAddress::Loopback(), 0); // to advise that address must be computed

	return true;
}

bool Socket::listen(Exception& ex, int backlog) {
	if (_ex) {
		ex = _ex;
		return false;
	}

	if (::listen(_id, backlog) == 0) {
		_listening = true;
		return true;
	}
	SetException(ex, Net::LastError()," (backlog=",backlog,")");
	return false;
}

int Socket::receive(Exception& ex, void* buffer, UInt32 size, int flags, SocketAddress* pAddress) {
	if (_ex) {
		ex = _ex;
		return -1;
	}
	
	int rc;
	int error;
	do {
		if (pAddress) {
			union {
				struct sockaddr_in  sa_in;
				struct sockaddr_in6 sa_in6;
			} addr;
			NET_SOCKLEN addrSize = sizeof(addr);
			if ((rc = ::recvfrom(_id, reinterpret_cast<char*>(buffer), size, flags, reinterpret_cast<sockaddr*>(&addr), &addrSize)) >= 0)
				pAddress->set(type == TYPE_STREAM ? peerAddress() : reinterpret_cast<const sockaddr&>(addr)); // check socket stream because WinSock doesn't assign correctly peerAddress on recvfrom for TCP socket
		} else
			rc = ::recv(_id, reinterpret_cast<char*>(buffer), size, flags);
	} while (rc < 0 && (error=Net::LastError()) == NET_EINTR);
		
	if (rc < 0) {
		if (error == NET_EAGAIN)
			error = NET_EWOULDBLOCK; // if non blocking socket keep returns -1 to differenciate it of disconnection which returns 0 (a non-blocking socket should use available before to call receive)
		if (pAddress)
			SetException(ex, error, " (from=", *pAddress,", size=", size, ", flags=", flags, ")");
		else if(_peerAddress)
			SetException(ex, error, " (from=",_peerAddress,", size=", size, ", flags=", flags, ")");
		else
			SetException(ex, error, " (size=", size, ", flags=", flags, ")");
		return -1;
	}
	
	if (!_address)
		_address.set(IPAddress::Loopback(), 0); // to advise that address is computable

	receive(rc);
	return rc;
}

int Socket::sendTo(Exception& ex, const void* data, UInt32 size, const SocketAddress& address, int flags) {
	if (_ex) {
		ex = _ex;
		return -1;
	}

#if defined(MSG_NOSIGNAL)
	flags |=  MSG_NOSIGNAL;
#endif

	int rc;
	int error;
	do {
		if (type == TYPE_DATAGRAM && address) // for TCP socket, address must be null!
			rc = ::sendto(_id, STR data, size, flags, address.data(), address.size());
		else
			rc = ::send(_id, STR data, size, flags);
	} while (rc < 0 && (error=Net::LastError()) == NET_EINTR);
	if (rc < 0) {
		if (error == NET_EAGAIN)
			error = NET_EWOULDBLOCK;
		SetException(ex, error, " (address=", address ? address : _peerAddress, ", size=", size, ", flags=", flags, ")");
		return -1;
	}
	
	if (!_address)
		_address.set(IPAddress::Loopback(), 0); // to advise that address is computable

	send(rc);

	if (UInt32(rc) < size && type == TYPE_DATAGRAM) {
		ex.set<Ex::Net::Socket>("UDP Packet sent in pieces (address=", address ? address : _peerAddress, ", size=", size, ", flags=", flags, ")");
		return -1;
	}

	return rc;
}

int Socket::write(Exception& ex, const Packet& packet, const SocketAddress& address, int flags) {
	lock_guard<mutex> lock(_mutexSending);
	if(!_sendings.empty()) {
		_sendings.emplace_back(packet, address ? address : _peerAddress, flags);
		_queueing += packet.size();
		return 0;
	}

	int	sent = sendTo(ex, packet.data(), packet.size(), address);
	if (sent < 0) {
		int code = ex.cast<Ex::Net::Socket>().code;
		if ((code == NET_ENOTCONN && _peerAddress) || code == NET_EWOULDBLOCK) {
			// queue and wait next call to flush(), no error!
			ex = nullptr;
			sent = 0;
		} else {
			// RELIABILITY IMPOSSIBLE => is not an error socket + is not connected (connecting = (peerAddress && error==NET_ENOTCONN) = false) + is not WOUldBLOCK
			if (type == TYPE_STREAM) { // else udp socket which send a packet without destinator address
				close(); // shutdown system to avoid to try to send before shutdown!
				_sendings.clear();
			}
			return -1;
		}
	} else if (UInt32(sent) >= packet.size())
		return packet.size();

	_sendings.emplace_back(packet+sent, address ? address : _peerAddress, flags);
	_queueing += _sendings.back().size();
	return sent;
}

bool Socket::flush(Exception& ex, bool deleting) {
	if (_ex) {
		ex = _ex;
		return false;
	}

	UInt32 written(0);

	unique_lock<mutex> lock(_mutexSending, defer_lock);
	if (!deleting)
		lock.lock();
	int sent(0);
	while(sent>=0 && !_sendings.empty()) {
		Sending& sending(_sendings.front());
		sent = sendTo(ex, sending.data(), sending.size(), sending.address, sending.flags);
		if (sent >= 0) {
			written += sent;
			if (UInt32(sent) < sending.size()) {
				// can't send more!
				sending += sent;
				break;
			}
		} else {
			int code = ex.cast<Ex::Net::Socket>().code;
			if ((code == NET_ENOTCONN && _peerAddress) || code == NET_EWOULDBLOCK) {
				// is connecting, can't send more now (wait onFlush)
				ex = nullptr;
				break;
			} else if (type == TYPE_STREAM) {
				// fail to send few reliable data, shutdown send!
				close(); // shutdown system to avoid to try to send before shutdown!
				_sendings.clear();
				return false;
			}
		}
		_sendings.pop_front();
	}
	if (deleting) {
		_sendings.clear();
		return true;
	}
	if (written)
		_queueing -= written;
	return true;
}



} // namespace Base
