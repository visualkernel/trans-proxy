//功能：一个简单透明代理服务器，主要针对tcp
//
//术语：
//    客户端：发起请求
//    代理端：本程序
//    服务端：客户端想要访问的真实目的端

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <linux/netfilter_ipv4.h>

typedef int bool;
#define true 1
#define false 0

//socket类型
enum proxy_socket_type {
	PROXY_SOCKET_LISTEN, //监听状态
	PROXY_SOCKET_CONN,   //连接状态
	PROXY_SOCKET_TRANS   //传输状态
};

//对原始socket一个简单封装，代理端维护的socket信息，用于保存事件监控过程中数据来往的状态
//    efd: 事件句柄，用于epoll的监控
//    type: socket类型，在不同类型的socket处理方式不同
//    from_fd: 代理端接收数据的socket
//    to_fd: 代理端将接收数据发送出去的socket
//    当客户端请求时:from_fd为与客户端连接的socket(pc_fd), to_fd为与服务端连接的socket(ps_fd)
//    当服务器响应时:from_fd为与服务端连接的socket(ps_fd), to_fd为与客户端连接的socket(pc_fd)
//    pc: proxy and client (代理端与客户端)
//　　ps: proxy and server (代理端与服务端)
//
//    epoll只对from_fd进行监控
//    在监听和传输状态，epoll监控from_fd的读(EPOLLIN)事件
//    在连接状态,epoll监控from_fd的读(EPOLLIN)与写(EPOLLOUT)事件(非阻塞connect函数的特性)
struct proxy_socket {
	int efd;             //监控事件句柄
	int type;            //socket类型(enum proxy_socket_type)
	int from_fd;         //数据来源
	int to_fd;           //数据目的
};

//----------------struct proxy_socket------------------------
struct proxy_socket *proxy_socket_create(int efd, int type, int from_fd, int to_fd)
{
	struct proxy_socket *sp = malloc(sizeof(struct proxy_socket));
	if (sp) {
		sp->efd = efd;
		sp->type = type;
		sp->from_fd = from_fd;
		sp->to_fd = to_fd;
	}

	return sp;
}

void proxy_socket_free(struct proxy_socket *s)
{
	free(s);
}

//----------------epoll event--------------------------------
int ev_create()
{
	return epoll_create(1024);
}

int ev_add(int efd, int fd, void *ud)
{
	struct epoll_event ev;

	ev.events = EPOLLIN;
	ev.data.ptr = ud;

	if (epoll_ctl(efd, EPOLL_CTL_ADD, fd, &ev) == -1) {
		return -1;
	}

	printf("add %d to event\n", fd);

	return 0;
}

//该函数激活fd的写事件监控
void ev_write(int efd, int fd, void *ud, bool enable)
{
	struct epoll_event ev;

	ev.events = EPOLLIN | (enable ? EPOLLOUT : 0);
	ev.data.ptr = ud;

	epoll_ctl(efd, EPOLL_CTL_MOD, fd, &ev);
}

void ev_del(int efd, int fd)
{
	epoll_ctl(efd, EPOLL_CTL_DEL, fd, NULL);

	printf("remove %d from event\n", fd);
}

int ev_wait(int efd, struct epoll_event *ev, int max)
{
	int n = epoll_wait(efd, ev, max, -1);

	return n;
}

int ev_close(int efd)
{
	close(efd);
}

void ev_nonblocking(int fd)
{
	int flag = fcntl(fd, F_GETFL, 0);
	if (flag < 0) {
		return;
	}

	fcntl(fd, F_SETFL, flag | O_NONBLOCK);
}

//------------------------proxy handle--------------------------
//代理端与服务端的建立连接
int proxy_connect_srv(struct proxy_socket *lis_socket, int pc_fd,
					  struct sockaddr_in *srv, socklen_t srv_len)
{
	int efd = lis_socket->efd;
	int ps_fd;  //proxy and server
	int err;
	
	ps_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (ps_fd < 0) {
		printf("socket error, %s\n", strerror(errno));
		return -1;
	}
	
	ev_nonblocking(ps_fd);

	err = connect(ps_fd, (struct sockaddr *)srv, srv_len);
	if (err < 0 && errno != EINPROGRESS) {//connect failed
		printf("connect error, %s\n", strerror(errno));
		goto err;
	} else if (err < 0) {//still connecting(未完成三次握手)
		printf("connect server %d ....\n", ps_fd);

		//如果仍在连接状态，则将socket添加到连接监控中
		struct proxy_socket *ps_socket = proxy_socket_create(efd, PROXY_SOCKET_CONN,
															 ps_fd, pc_fd);
		if (ps_socket == NULL) {
			goto err;
		}
		
		ev_add(efd, ps_fd, ps_socket);
		ev_write(efd, ps_fd, ps_socket, true);//对于连接的监控，必须激活写事件(*)
	} else {//connect success

		printf("connect server %d\n", ps_fd);

		//如果已经完成连接，则将两个socket都添加到传输监控中，处理接下来的数据传输
		struct proxy_socket *ps_socket = proxy_socket_create(efd, PROXY_SOCKET_TRANS,
															 ps_fd, pc_fd);
		struct proxy_socket *pc_socket = proxy_socket_create(efd, PROXY_SOCKET_TRANS,
															 pc_fd, ps_fd);
		if (ps_socket && pc_socket) {
			ev_add(efd, ps_fd, ps_socket);
			ev_add(efd, pc_fd, pc_socket);

			printf("create proxy, %d -> %d\n", pc_fd, ps_fd);
		} else {
			proxy_socket_free(ps_socket);
			proxy_socket_free(pc_socket);

			goto err;
		}

	}

	return 0;

err:
	close(ps_fd);
	return -1; 
}


//该函数完成代理端向服务端的发起连接
void proxy_create(struct proxy_socket *lis_socket, int pc_fd)
{
	struct sockaddr_in dst;
	int dst_len;
	int err;
	int efd = lis_socket->efd;

	/* 获取服务端的地址信息(透明代理) */
	dst_len = sizeof(dst);
	err = getsockopt(pc_fd, SOL_IP, SO_ORIGINAL_DST, (struct sockaddr*)&dst, &dst_len);
	if (err < 0) {
		printf("getsockopt(SOL_IP, SO_ORIGINAL_DST) error, %s\n", strerror(errno)); 
		goto err;
	}
	
	printf("You got real dst: %s : %d\n",
	       inet_ntoa(dst.sin_addr), ntohs(dst.sin_port));

	/* 连接服务端 */	
	err = proxy_connect_srv(lis_socket, pc_fd, &dst, dst_len);
	if (err < 0) {
		goto err;
	}

	return;

err:
	close(pc_fd);
}


//处理客户端发起的连接请求事件,完成代理连接工作
//该函数主要完成2件事：
//    1. 完成与客户端建立连接
//    2. 向真实服务端发起连接请求
void proxy_accept(struct proxy_socket *lis_socket)
{
	struct sockaddr_in cli; 
	int cli_len = sizeof(cli);
	int pc_fd;

	//接收客户端的连接请求
	pc_fd = accept(lis_socket->from_fd, (struct sockaddr*)&cli, &cli_len);
	if (pc_fd < 0) {
		printf("accept error, %s\n", strerror(errno));
		return;
	}

	ev_nonblocking(pc_fd);//非阻塞
	
	printf("accept client %d\n", pc_fd);

	//完成代理端与服务端的连接工作
	proxy_create(lis_socket, pc_fd);
}

//处理代理端与服务端连接事件，当连接完成时触发
void proxy_conn(struct proxy_socket *ps_socket)
{
	int efd = ps_socket->efd;
	int ps_fd = ps_socket->from_fd;
	int pc_fd = ps_socket->to_fd;
	int error = 1;
	int len = sizeof(error);

	//判断建立连接过程是否成功
	if (getsockopt(ps_fd, SOL_SOCKET, SO_ERROR, &error, &len) < 0) {
		goto err;
	}

	if (error) {//connect failed
		goto err;
	}


	//将当前ps_ocket(服务端的socket)状态转换为传输
	ps_socket->type = PROXY_SOCKET_TRANS;

	//添加客户端的数据传输监控
	struct proxy_socket *pc_socket = proxy_socket_create(efd, PROXY_SOCKET_TRANS,
														 pc_fd, ps_fd);
	if (pc_socket == NULL) {
		goto err;
	}
		
	ev_add(efd, pc_fd, pc_socket);
	ev_write(efd, ps_fd, socket, false);//清理写事件监控(因为写事件只有连接监控时有用)

	printf("connect server %d success\n", ps_fd);
	
	return;
err:
	ev_del(efd, ps_fd);
	close(ps_fd);
	close(ps_fd);
	proxy_socket_free(ps_socket);

	printf("connect server %d failed\n", ps_fd);
	printf("close server %d\n", ps_fd);
	printf("close client %d\n", pc_fd);
}

//处理客户端或服务端的数据传输，当接收到数据时触发
void proxy_trans(struct proxy_socket *socket)
{
	int from_fd = socket->from_fd;
	int to_fd = socket->to_fd;
	int efd = socket->efd;
	char buf[2048];

	memset(buf, 0, sizeof(buf));

	//从from_fd读取数据，发送给to_fd
	int n = read(from_fd, buf, sizeof(buf));
	if (n <= 0) {
		ev_del(efd, from_fd); 
		close(from_fd);
		proxy_socket_free(socket);

		printf("connect %d closed\n", from_fd);
	} else {
		write(to_fd, buf, n);

		printf("send %d bytes data from %d to %d\n", n, from_fd, to_fd);
	}
}

//封装了服务器的创建过程
int server(short port)
{
	int sock;
	struct sockaddr_in srv;
	int err;
	
	sock = socket(AF_INET, SOCK_STREAM, 0);
	if (sock < 0) {
		printf("socket error, %s\n", strerror(errno));
		return -1;
	}

	//设置IP_TRANSPARENT,能够在透明代理时获取服务端地址信息
	int opt = 1;
	err = setsockopt(sock, SOL_IP, IP_TRANSPARENT, &opt, sizeof(opt));
	if (err < 0) {
		printf("setsockopt error, %s\n", strerror(errno));
		return -1;
	}

	//本地socket地址重用
	opt = 1;
	err = setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
	if (err < 0) {
		printf("setsockopt error, %s\n", strerror(errno));
		return -1;
	}
	
	srv.sin_family = AF_INET;
	srv.sin_port = htons(port);
	srv.sin_addr.s_addr = htonl(INADDR_ANY);

	//socket地址绑定
	err = bind(sock, (struct sockaddr *)&srv, sizeof(srv));
	if (err < 0) {
		printf("bind error, %s\n", strerror(errno));
		return -1;
	}

	//启动监听
	err = listen(sock, 10);
	if (err < 0) {
		printf("listen error, %s\n", strerror(errno));
		return -1;
	}

	return sock;
}
	

int main(int argc, char **argv)
{
	int lis_fd;
	int efd;
	struct epoll_event evs[1024];
	struct proxy_socket *lis_socket;

	//创建代理端的服务
	lis_fd = server(3128);
	if (lis_fd < 0) {
		printf("create server faild\n");
		return -1;
	}

	//创建事件监控句柄
	efd = ev_create();
	if (efd < 0) return -1;
	
	lis_socket = proxy_socket_create(efd, PROXY_SOCKET_LISTEN, lis_fd, -1);
	if (lis_socket == NULL) {
		close(lis_fd);
		return -1;
	}
	
	//将监听socket添加到事件监控表中
	ev_add(efd, lis_fd, lis_socket);

	for(;;) {
		//监控添加的所有Socket,等待事件的来临
		//该函数一直处于阻塞状态直到事件到来
		int n = ev_wait(efd, evs, 1024);
		int i;
		for (i = 0; i < n; i++) {
			struct proxy_socket *sp = (struct proxy_socket *)evs[i].data.ptr;
			if (sp == NULL) continue;

			if (sp->type == PROXY_SOCKET_LISTEN) {
				//监听事件，表示有客户端发起了连接
				printf("--------------%d event comming(accept, sp: %p)---------------\n",
				       sp->from_fd, sp); 
				proxy_accept(sp);
			} else if (sp->type == PROXY_SOCKET_CONN) {
				//连接事件，表示建立连接的过程已经完成(针对proxy向服务器connect())
				printf("--------------%d event comming(conn, sp: %p)---------------\n",
				       sp->from_fd, sp);
				proxy_conn(sp); 
			} else if (sp->type == PROXY_SOCKET_TRANS) {
				//传输事件，表示客户端或服务器发送业务数据
				printf("--------------%d event comming(trans, sp: %p)---------------\n",
				       sp->from_fd, sp);
				proxy_trans(sp);
			}
		}
	}

	ev_del(efd, lis_fd);
	close(lis_fd);
	proxy_socket_free(lis_socket);
	ev_close(efd);
	
	return 0;
}
