/*
 * Copyright (c) 2007 Niels Provos <provos@citi.umich.edu>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef WIN32
#include <winsock2.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#undef WIN32_LEAN_AND_MEAN
#endif

#include <sys/types.h>
#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif
#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif
#include <errno.h>
#if defined WIN32 && !defined(HAVE_GETTIMEOFDAY_H)
#include <sys/timeb.h>
#endif
#include <stdio.h>
#include <signal.h>

#include <sys/queue.h>
#include "event.h"
#include "event-internal.h"
#include "evutil.h"
#include "log.h"
/**
 * 在evsignal_init中调用，evutil_socketpair(AF_UNIX, SOCK_STREAM, 0, base->sig.ev_signal_pair)用来创建一对socket，读和写
*/
int
evutil_socketpair(int family, int type, int protocol, int fd[2])
{
#ifndef WIN32
	return socketpair(family, type, protocol, fd);
#else
	/* This code is originally from Tor.  Used with permission. */

	/* This socketpair does not work when localhost is down. So
	 * it's really not the same thing at all. But it's close enough
	 * for now, and really, when localhost is down sometimes, we
	 * have other problems too.
	 */
	int listener = -1;
	int connector = -1;
	int acceptor = -1;
	struct sockaddr_in listen_addr;
	struct sockaddr_in connect_addr;
	int size;
	int saved_errno = -1;

	if (protocol
#ifdef AF_UNIX
		|| family != AF_UNIX
#endif
		) {
		EVUTIL_SET_SOCKET_ERROR(WSAEAFNOSUPPORT);
		return -1;
	}
	if (!fd) {
		EVUTIL_SET_SOCKET_ERROR(WSAEINVAL);
		return -1;
	}
    /**
	 * int socket(int domain, int type, int protocol);
	　　在参数表中，domain指定使用何种的地址类型，比较常用的有：
	　　PF_INET, AF_INET： Ipv4网络协议；
	　　PF_INET6, AF_INET6： Ipv6网络协议。
	　　type参数的作用是设置通信的协议类型，可能的取值如下所示：
	　　SOCK_STREAM： 提供面向连接的稳定数据传输，即TCP协议。
	　　OOB： 在所有数据传送前必须使用connect()来建立连接状态。
	　　SOCK_DGRAM： 使用不连续不可靠的数据包连接。UPD协议
	　　SOCK_SEQPACKET： 提供连续可靠的数据包连接。
	　　SOCK_RAW： 提供原始网络协议存取。
	　　SOCK_RDM： 提供可靠的数据包连接。
	　　SOCK_PACKET： 与网络驱动程序直接通信。e79fa5e98193e4b893e5b19e31333330336335
	　　参数protocol用来指定socket所使用的传输协议编号。这一参数通常不具体设置，一般设置为0即可。
	*/
    //socket(AF_INET, SOCK_STREAM, 0);创建socket,IPV4,TCP协议
	//socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP) 这个是创建UDP协议的socket
	listener = socket(AF_INET, type, 0);
	if (listener < 0)
		return -1;
	//清空sockaddr_in结构体
	memset(&listen_addr, 0, sizeof(listen_addr));
	listen_addr.sin_family = AF_INET;
	//INADDR_LOOPBACK, 也就是绑定地址LOOPBAC, 往往是127.0.0.1, 只能收到127.0.0.1上面的连接请求
	listen_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	listen_addr.sin_port = 0;	/* kernel chooses port.	 */
	//绑定socket和监听listen_addr
	if (bind(listener, (struct sockaddr *) &listen_addr, sizeof (listen_addr))
		== -1)
		goto tidy_up_and_fail;
    //启动监听
	if (listen(listener, 1) == -1)
		goto tidy_up_and_fail;
    
	//相当于客户端socket
	connector = socket(AF_INET, type, 0);
	if (connector < 0)
		goto tidy_up_and_fail;
	/* We want to find out the port number to connect to.  */
	size = sizeof(connect_addr);
	//getsockname获取一个套接口的名字
	if (getsockname(listener, (struct sockaddr *) &connect_addr, &size) == -1)
		goto tidy_up_and_fail;
	if (size != sizeof (connect_addr))
		goto abort_tidy_up_and_fail;
	//启动连接
	if (connect(connector, (struct sockaddr *) &connect_addr,
				sizeof(connect_addr)) == -1)
		goto tidy_up_and_fail;

	size = sizeof(listen_addr);
	//连接，返回连接后的socket
	acceptor = accept(listener, (struct sockaddr *) &listen_addr, &size);
	if (acceptor < 0)
		goto tidy_up_and_fail;
	if (size != sizeof(listen_addr))
		goto abort_tidy_up_and_fail;
	EVUTIL_CLOSESOCKET(listener);
	/* Now check we are talking to ourself by matching port and host on the
	   two sockets.	 */
	if (getsockname(connector, (struct sockaddr *) &connect_addr, &size) == -1)
		goto tidy_up_and_fail;
	if (size != sizeof (connect_addr)
		|| listen_addr.sin_family != connect_addr.sin_family
		|| listen_addr.sin_addr.s_addr != connect_addr.sin_addr.s_addr
		|| listen_addr.sin_port != connect_addr.sin_port)
		goto abort_tidy_up_and_fail;
	fd[0] = connector;
	fd[1] = acceptor;

	return 0;

 abort_tidy_up_and_fail:
	saved_errno = WSAECONNABORTED;
 tidy_up_and_fail:
	if (saved_errno < 0)
		saved_errno = WSAGetLastError();
	if (listener != -1)
		EVUTIL_CLOSESOCKET(listener);
	if (connector != -1)
		EVUTIL_CLOSESOCKET(connector);
	if (acceptor != -1)
		EVUTIL_CLOSESOCKET(acceptor);

	EVUTIL_SET_SOCKET_ERROR(saved_errno);
	return -1;
#endif
}

int
evutil_make_socket_nonblocking(int fd)
{
#ifdef WIN32
	{
		unsigned long nonblocking = 1;
		ioctlsocket(fd, FIONBIO, (unsigned long*) &nonblocking);
	}
#else
	if (fcntl(fd, F_SETFL, O_NONBLOCK) == -1) {
		event_warn("fcntl(O_NONBLOCK)");
		return -1;
}	
#endif
	return 0;
}

ev_int64_t
evutil_strtoll(const char *s, char **endptr, int base)
{
#ifdef HAVE_STRTOLL
	return (ev_int64_t)strtoll(s, endptr, base);
#elif SIZEOF_LONG == 8
	return (ev_int64_t)strtol(s, endptr, base);
#elif defined(WIN32) && defined(_MSC_VER) && _MSC_VER < 1300
	/* XXXX on old versions of MS APIs, we only support base
	 * 10. */
	ev_int64_t r;
	if (base != 10)
		return 0;
	r = (ev_int64_t) _atoi64(s);
	while (isspace(*s))
		++s;
	while (isdigit(*s))
		++s;
	if (endptr)
		*endptr = (char*) s;
	return r;
#elif defined(WIN32)
	return (ev_int64_t) _strtoi64(s, endptr, base);
#else
#error "I don't know how to parse 64-bit integers."
#endif
}

#ifndef _EVENT_HAVE_GETTIMEOFDAY
int
evutil_gettimeofday(struct timeval *tv, struct timezone *tz)
{
	struct _timeb tb;

	if(tv == NULL)
		return -1;

	_ftime(&tb);
	tv->tv_sec = (long) tb.time;
	tv->tv_usec = ((int) tb.millitm) * 1000;
	return 0;
}
#endif

int
evutil_snprintf(char *buf, size_t buflen, const char *format, ...)
{
	int r;
	va_list ap;
	va_start(ap, format);
	r = evutil_vsnprintf(buf, buflen, format, ap);
	va_end(ap);
	return r;
}

int
evutil_vsnprintf(char *buf, size_t buflen, const char *format, va_list ap)
{
#ifdef _MSC_VER
	int r = _vsnprintf(buf, buflen, format, ap);
	buf[buflen-1] = '\0';
	if (r >= 0)
		return r;
	else
		return _vscprintf(format, ap);
#else
	int r = vsnprintf(buf, buflen, format, ap);
	buf[buflen-1] = '\0';
	return r;
#endif
}

static int
evutil_issetugid(void)
{
#ifdef _EVENT_HAVE_ISSETUGID
	return issetugid();
#else

#ifdef _EVENT_HAVE_GETEUID
	if (getuid() != geteuid())
		return 1;
#endif
#ifdef _EVENT_HAVE_GETEGID
	if (getgid() != getegid())
		return 1;
#endif
	return 0;
#endif
}

const char *
evutil_getenv(const char *varname)
{
	if (evutil_issetugid())
		return NULL;

	return getenv(varname);
}
