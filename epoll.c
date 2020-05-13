/*
 * Copyright 2000-2003 Niels Provos <provos@citi.umich.edu>
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

#include <stdint.h>
#include <sys/types.h>
#include <sys/resource.h>
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#else
#include <sys/_libevent_time.h>
#endif
#include <sys/queue.h>
#include <sys/epoll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif

#include "event.h"
#include "event-internal.h"
#include "evsignal.h"
#include "log.h"

/* due to limitations in the epoll interface, we need to keep track of
 * all file descriptors outself.
 */
struct evepoll {
	struct event *evread;
	struct event *evwrite;
};

struct epollop {
	struct evepoll *fds;
	int nfds;
	struct epoll_event *events;
	int nevents;
	int epfd;
};

static void *epoll_init	(struct event_base *);
static int epoll_add	(void *, struct event *);
static int epoll_del	(void *, struct event *);
static int epoll_dispatch	(struct event_base *, void *, struct timeval *);
static void epoll_dealloc	(struct event_base *, void *);

const struct eventop epollops = {
	"epoll",
	epoll_init,
	epoll_add,
	epoll_del,
	epoll_dispatch,
	epoll_dealloc,
	1 /* need reinit */
};

//执行fcntl(epfd, F_SETFD, 1)，第三个参数FD_CLOEXEC为1表示：调用exec相关函数后文件句柄将被关闭
#ifdef HAVE_SETFD
#define FD_CLOSEONEXEC(x) do { \
        if (fcntl(x, F_SETFD, 1) == -1) \
                event_warn("fcntl(%d, F_SETFD)", x); \
} while (0)
#else
#define FD_CLOSEONEXEC(x)
#endif

/* On Linux kernels at least up to 2.6.24.4, epoll can't handle timeout
 * values bigger than (LONG_MAX - 999ULL)/HZ.  HZ in the wild can be
 * as big as 1000, and LONG_MAX can be as small as (1<<31)-1, so the
 * largest number of msec we can support here is 2147482.  Let's
 * round that down by 47 seconds.
 */
#define MAX_EPOLL_TIMEOUT_MSEC (35*60*1000)

#define INITIAL_NFILES 32
#define INITIAL_NEVENTS 32
#define MAX_NEVENTS 4096

static void *
epoll_init(struct event_base *base)
{
	int epfd;
	struct epollop *epollop;

	/* Disable epollueue when this environment variable is set */
	if (evutil_getenv("EVENT_NOEPOLL"))
		return (NULL);

	/* Initalize the kernel queue */
	if ((epfd = epoll_create(32000)) == -1) {
		if (errno != ENOSYS)
			event_warn("epoll_create");
		return (NULL);
	}
    //执行fcntl(epfd, F_SETFD, 1)，第三个参数FD_CLOEXEC为1表示：调用exec相关函数后文件句柄将被关闭
	FD_CLOSEONEXEC(epfd);

	if (!(epollop = calloc(1, sizeof(struct epollop))))
		return (NULL);

	epollop->epfd = epfd;

	/* Initalize fields */
	epollop->events = malloc(INITIAL_NEVENTS * sizeof(struct epoll_event));
	if (epollop->events == NULL) {
		free(epollop);
		return (NULL);
	}
	epollop->nevents = INITIAL_NEVENTS;

	epollop->fds = calloc(INITIAL_NFILES, sizeof(struct evepoll));
	if (epollop->fds == NULL) {
		free(epollop->events);
		free(epollop);
		return (NULL);
	}
	epollop->nfds = INITIAL_NFILES;
    //初始化socket pair
	evsignal_init(base);

	return (epollop);
}

static int
epoll_recalc(struct event_base *base, void *arg, int max)
{
	struct epollop *epollop = arg;

	if (max >= epollop->nfds) {
		struct evepoll *fds;
		int nfds;

		nfds = epollop->nfds;
		while (nfds <= max)
			nfds <<= 1;

		fds = realloc(epollop->fds, nfds * sizeof(struct evepoll));
		if (fds == NULL) {
			event_warn("realloc");
			return (-1);
		}
		epollop->fds = fds;
		memset(fds + epollop->nfds, 0,
		    (nfds - epollop->nfds) * sizeof(struct evepoll));
		epollop->nfds = nfds;
	}

	return (0);
}

static int
epoll_dispatch(struct event_base *base, void *arg, struct timeval *tv)
{
	struct epollop *epollop = arg;
	struct epoll_event *events = epollop->events;
	struct evepoll *evep;
	int i, res, timeout = -1;

	if (tv != NULL)
	    //超时时间，单位ms,向上取整
		timeout = tv->tv_sec * 1000 + (tv->tv_usec + 999) / 1000;

	if (timeout > MAX_EPOLL_TIMEOUT_MSEC) {
		/* Linux kernels can wait forever if the timeout is too big;
		 * see comment on MAX_EPOLL_TIMEOUT_MSEC. */
		timeout = MAX_EPOLL_TIMEOUT_MSEC;
	}
    //events是用来接收返回的事件集合，res是集合中元素的个数，epollop->nevents表示events的容量，不能大于创建epoll_create()时的size
	res = epoll_wait(epollop->epfd, events, epollop->nevents, timeout);

	if (res == -1) {  //返回-1,说明epoll_wait产生异常
		if (errno != EINTR) {  //在超时或监听到事件发生前收到的不是中断信号
			event_warn("epoll_wait");
			return (-1);
		}

		evsignal_process(base); // 处理signal事件 
		return (0);
	} else if (base->sig.evsignal_caught) {    //监听的某种信号产生并处理
		evsignal_process(base); // 处理signal事件 
	}

	event_debug(("%s: epoll_wait reports %d", __func__, res));
    //events[i].events表示事件类型，events[i].data.fd表示句柄
	for (i = 0; i < res; i++) {
		int what = events[i].events;
		struct event *evread = NULL, *evwrite = NULL;
		int fd = events[i].data.fd;    //发生该事件的文件描述符

		if (fd < 0 || fd >= epollop->nfds)  
			continue;
		evep = &epollop->fds[fd];  //发生该事件的文件描述符的事件类型（2个事件类型读、写）
        //该文件描述符被挂断或发生错误（读事件应该是用来注销该文件描述符上的事件，这里不明白激活写事件是要干嘛）
		if (what & (EPOLLHUP|EPOLLERR)) {
			evread = evep->evread;
			evwrite = evep->evwrite;
		} else {
			if (what & EPOLLIN) {  //该文件描述符可以读（包括对端SOCKET正常关闭）
				evread = evep->evread;  //记录这个读事件
			}

			if (what & EPOLLOUT) {   //文件描述符可以写
				evwrite = evep->evwrite;   //记录这个写事件
			}
		}

		if (!(evread||evwrite))   //如果没有读或写事件发生，跳过该文件描述符的事件
			continue;
        
		if (evread != NULL)      //如果前面记录了读事件，将该事件插入到激活链表，且调用事件回调次数设为1
			event_active(evread, EV_READ, 1);
		if (evwrite != NULL)    //如果前面记录了读事件，将该事件插入到激活链表，且调用事件回调次数设为1
			event_active(evwrite, EV_WRITE, 1);
	}

	if (res == epollop->nevents && epollop->nevents < MAX_NEVENTS) {   //监听到的事件数量达到了分配的缓存上限，但没有达到MAX_NEVENTS，说明我们的缓存数组不够用了，扩充缓存监听事件的数组
		/* We used all of the event space this time.  We should
		   be ready for more events next time. */
		int new_nevents = epollop->nevents * 2;
		struct epoll_event *new_events;

		new_events = realloc(epollop->events,
		    new_nevents * sizeof(struct epoll_event));
		if (new_events) {
			epollop->events = new_events;
			epollop->nevents = new_nevents;
		}
	}

	return (0);
}

//参数1：base->evbase
static int
epoll_add(void *arg, struct event *ev)
{
	struct epollop *epollop = arg;
	struct epoll_event epev = {0, {0}};
	struct evepoll *evep;
	int fd, op, events;

	if (ev->ev_events & EV_SIGNAL)
		return (evsignal_add(ev));

	fd = ev->ev_fd;
	//如果这个要注册事件的文件描述符值超过已分配的文件描述符数组长度，调用realloc扩充epollop->fds的分配内存×2，直到这个值小于数组长度。将扩充部分置为0
	if (fd >= epollop->nfds) {
		/* Extent the file descriptor array as necessary */
		if (epoll_recalc(ev->ev_base, epollop, fd) == -1)
			return (-1);
	}
	evep = &epollop->fds[fd]; //这里返回epollop的内存，该内存初始为空，会在后面赋值ev
	op = EPOLL_CTL_ADD;
	events = 0;
	//该文件描述符已经存在读事件，修改时保留读事件
	if (evep->evread != NULL) {
		events |= EPOLLIN;
		op = EPOLL_CTL_MOD;
	}
	//该文件描述符已经存在写事件，修改时保留写事件
	if (evep->evwrite != NULL) {
		events |= EPOLLOUT;
		op = EPOLL_CTL_MOD;
	}
    //注册事件如果需要注册读事件，为该文件描述符添加读事件
	if (ev->ev_events & EV_READ)
		events |= EPOLLIN;
	if (ev->ev_events & EV_WRITE)
		events |= EPOLLOUT;
    //epoll_event结构体设置两个值，epev.data.fd为event中保存的句柄，epev.events为事件类型
	epev.data.fd = fd;   //监听的文件描述符
	epev.events = events; //需要监听改文件描述符发生的事件
	//将该文件描述符需要监听的事件注册到epoll
	if (epoll_ctl(epollop->epfd, op, ev->ev_fd, &epev) == -1)
			return (-1);

	/* Update events responsible */
	if (ev->ev_events & EV_READ)
		evep->evread = ev;  //表明该文件描述符有读事件
	if (ev->ev_events & EV_WRITE)
		evep->evwrite = ev; //表明该文件描述符有写事件

	return (0);
}

static int
epoll_del(void *arg, struct event *ev)
{
	struct epollop *epollop = arg;
	struct epoll_event epev = {0, {0}};
	struct evepoll *evep;
	int fd, events, op;
	int needwritedelete = 1, needreaddelete = 1;

	if (ev->ev_events & EV_SIGNAL)
		return (evsignal_del(ev));

	fd = ev->ev_fd;
	if (fd >= epollop->nfds)
		return (0);
	evep = &epollop->fds[fd];

	op = EPOLL_CTL_DEL;
	events = 0;

	if (ev->ev_events & EV_READ)
		events |= EPOLLIN;
	if (ev->ev_events & EV_WRITE)
		events |= EPOLLOUT;

	if ((events & (EPOLLIN|EPOLLOUT)) != (EPOLLIN|EPOLLOUT)) {
		if ((events & EPOLLIN) && evep->evwrite != NULL) {
			needwritedelete = 0;
			events = EPOLLOUT;
			op = EPOLL_CTL_MOD;
		} else if ((events & EPOLLOUT) && evep->evread != NULL) {
			needreaddelete = 0;
			events = EPOLLIN;
			op = EPOLL_CTL_MOD;
		}
	}

	epev.events = events;
	epev.data.fd = fd;

	if (needreaddelete)
		evep->evread = NULL;
	if (needwritedelete)
		evep->evwrite = NULL;

	if (epoll_ctl(epollop->epfd, op, fd, &epev) == -1)
		return (-1);

	return (0);
}

static void
epoll_dealloc(struct event_base *base, void *arg)
{
	struct epollop *epollop = arg;

	evsignal_dealloc(base);
	if (epollop->fds)
		free(epollop->fds);
	if (epollop->events)
		free(epollop->events);
	if (epollop->epfd >= 0)
		close(epollop->epfd);

	memset(epollop, 0, sizeof(struct epollop));
	free(epollop);
}
