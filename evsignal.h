/*
 * Copyright 2000-2002 Niels Provos <provos@citi.umich.edu>
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
#ifndef _EVSIGNAL_H_
#define _EVSIGNAL_H_

typedef void (*ev_sighandler_t)(int);

struct evsignal_info {
	//为 socket pair 的读 socket 向 event_base 注册读事件时使用的 event 结构体； 
	struct event ev_signal;
	//是一个 socket 对，包含两个 socket，一个读 socket，一个写 socket
	int ev_signal_pair[2];
	//记录 ev_signal 事件是否已经注册了； 
	int ev_signal_added;
	//是否有信号发生的标记；是 volatile 类型，因为它会在另外的线程中被 修改
	volatile sig_atomic_t evsignal_caught;
	//数组，evsigevents[signo]表示注册到信号 signo 的事件链表； 
	struct event_list evsigevents[NSIG];
	//具体记录每个信号触发的次数，evsigcaught[signo]是记录信号 signo 被触发的次数
	sig_atomic_t evsigcaught[NSIG];
#ifdef HAVE_SIGACTION
	struct sigaction **sh_old;
#else
    //记录了原来的 signal 处理函数指针，当信号 signo 注册的 event 被清空时，需要重 新设置其处理函数
	ev_sighandler_t **sh_old;
#endif
	int sh_old_max;
};
int evsignal_init(struct event_base *);
void evsignal_process(struct event_base *);
int evsignal_add(struct event *);
int evsignal_del(struct event *);
void evsignal_dealloc(struct event_base *);

#endif /* _EVSIGNAL_H_ */
