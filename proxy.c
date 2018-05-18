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

enum proxy_socket_type {
	PROXY_SOCKET_LISTEN,
	PROXY_SOCKET_CONN,
	PROXY_SOCKET_TRANS
};

struct proxy_socket {
	int efd;
	int type; 
	int from_fd;
	int to_fd;
};

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

int proxy_connect_srv(struct proxy_socket *lis_socket, int cli_fd, struct sockaddr_in *srv, socklen_t srv_len)
{
	int efd = lis_socket->efd;
	int srv_fd;
	int err;
	
	srv_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (srv_fd < 0) {
		printf("socket error, %s\n", strerror(errno));
		return -1;
	}
	
	ev_nonblocking(srv_fd);

	err = connect(srv_fd, (struct sockaddr *)srv, srv_len);
	if (err < 0 && errno != EINPROGRESS) {//connect failed
		printf("connect error, %s\n", strerror(errno));
		goto err;
	} else if (err < 0) {//still connecting
		printf("connect server %d ....\n", srv_fd);
		
		struct proxy_socket *srv_socket = proxy_socket_create(efd, PROXY_SOCKET_CONN,
								      srv_fd, cli_fd);
		if (srv_socket == NULL) {
			goto err;
		}
		
		ev_add(efd, srv_fd, srv_socket);
		ev_write(efd, srv_fd, srv_socket, true);
	} else {//connect success

		printf("connect server %d\n", srv_fd);
		
		struct proxy_socket *srv_socket = proxy_socket_create(efd, PROXY_SOCKET_TRANS,
								      srv_fd, cli_fd);
		struct proxy_socket *cli_socket = proxy_socket_create(efd, PROXY_SOCKET_TRANS,
								      cli_fd, srv_fd);
		if (srv_socket && cli_socket) {
			ev_add(efd, srv_fd, srv_socket);
			ev_add(efd, cli_fd, cli_socket);

			printf("create proxy, %d -> %d\n", cli_fd, srv_fd);
		} else {
			proxy_socket_free(srv_socket);
			proxy_socket_free(cli_socket);

			goto err;
		}

	}

	return 0;

err:
	close(srv_fd);
	return -1; 
}


void proxy_create(struct proxy_socket *lis_socket, int cli_fd)
{
	struct sockaddr_in dst;
	int dst_len;
	int err;
	int efd = lis_socket->efd;

	/* get original server address */
	dst_len = sizeof(dst);
	err = getsockopt(cli_fd, SOL_IP, SO_ORIGINAL_DST, (struct sockaddr*)&dst, &dst_len);
	if (err < 0) {
		printf("getsockopt(SOL_IP, SO_ORIGINAL_DST) error, %s\n", strerror(errno)); 
		goto err;
	}
	
	printf("You got real dst: %s : %d\n",
	       inet_ntoa(dst.sin_addr), ntohs(dst.sin_port));

	/* connect to server */	
	err = proxy_connect_srv(lis_socket, cli_fd, &dst, dst_len);
	if (err < 0) {
		goto err;
	}

	return;

err:
	close(cli_fd);
}

void proxy_accept(struct proxy_socket *lis_socket)
{
	struct sockaddr_in cli;
	int cli_fd;
	int cli_len = sizeof(cli);
				
	cli_fd = accept(lis_socket->from_fd, (struct sockaddr*)&cli, &cli_len);
	if (cli_fd < 0) {
		printf("accept error, %s\n", strerror(errno));
		return;
	}

	ev_nonblocking(cli_fd);

	printf("accept client %d\n", cli_fd);
				
	proxy_create(lis_socket, cli_fd);
}

void proxy_conn(struct proxy_socket *socket)
{
	int efd = socket->efd;
	int from_fd = socket->from_fd;
	int to_fd = socket->to_fd;
	int error = 1;
	int len = sizeof(error);
	
	if (getsockopt(socket->from_fd, SOL_SOCKET, SO_ERROR, &error, &len) < 0) {
		goto err;
	}

	if (error) {//connect failed
		goto err;
	}
	
	socket->type = PROXY_SOCKET_TRANS;

	struct proxy_socket *cli_socket = proxy_socket_create(efd, PROXY_SOCKET_TRANS,
							      to_fd, from_fd);
	if (cli_socket == NULL) {
		goto err;
	}
		
	ev_add(efd, to_fd, cli_socket);
	ev_write(efd, from_fd, socket, false);

	printf("connect server %d success\n", from_fd);
	
	return;
err:
	ev_del(efd, from_fd);
	close(from_fd);
	close(to_fd);
	proxy_socket_free(socket);

	printf("connect server %d failed\n", from_fd);
	printf("close server %d\n", from_fd);
	printf("close client %d\n", to_fd);
}

void proxy_trans(struct proxy_socket *socket)
{
	int from_fd = socket->from_fd;
	int to_fd = socket->to_fd;
	int efd = socket->efd;
	char buf[2048];

	memset(buf, 0, sizeof(buf));
	
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

	//设置IP_TRANSPARENT,满足透明代理时获取目的ip的能力
	int opt = 1;
	err = setsockopt(sock, SOL_IP, IP_TRANSPARENT, &opt, sizeof(opt));
	if (err < 0) {
		printf("setsockopt error, %s\n", strerror(errno));
		return -1;
	}

	opt = 1;
	err = setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
	if (err < 0) {
		printf("setsockopt error, %s\n", strerror(errno));
		return -1;
	}
	
	srv.sin_family = AF_INET;
	srv.sin_port = htons(port);
	srv.sin_addr.s_addr = htonl(INADDR_ANY);

	err = bind(sock, (struct sockaddr *)&srv, sizeof(srv));
	if (err < 0) {
		printf("bind error, %s\n", strerror(errno));
		return -1;
	}

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
	
	lis_fd = server(3128);
	if (lis_fd < 0) {
		printf("create server faild\n");
		return -1;
	}

	efd = ev_create();
	if (efd < 0) return -1;
	
	lis_socket = proxy_socket_create(efd, PROXY_SOCKET_LISTEN, lis_fd, -1);
	if (lis_socket == NULL) {
		close(lis_fd);
		return -1;
	}
	
	//将监听socket添加到事件监控队列表中
	ev_add(efd, lis_fd, lis_socket);

	for(;;) {
		//监控队列中的所有Socket,等待事件的来临
		//该函数一直处于阻塞状态直到事件到来
		int n = ev_wait(efd, evs, 1024);
		int i;
		for (i = 0; i < n; i++) {
			struct proxy_socket *sp = (struct proxy_socket *)evs[i].data.ptr;
			if (sp == NULL) continue;

		 	//监听事件	
			if (sp->type == PROXY_SOCKET_LISTEN) {
				printf("--------------%d event comming(accept, sp: %p)---------------\n",
				       sp->from_fd, sp); 
				proxy_accept(sp);
			//连接事件
			} else if (sp->type == PROXY_SOCKET_CONN) {
				printf("--------------%d event comming(conn, sp: %p)---------------\n",
				       sp->from_fd, sp);
				proxy_conn(sp);
			//传输事件
			} else if (sp->type == PROXY_SOCKET_TRANS) {
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
