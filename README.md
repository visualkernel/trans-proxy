## trans-proxy透明代理
- 代理：
  - 正向代理：代理客户端来访问外部资源，需要客户端设置代理服务器
  - 反向代理：代理服务器，接受客户端请求，需要客户端设置代理服务器
  - 透明代理：被代理的对象无需配置，可进行无感知的代理 
    

- 正向透明代理
  - 顾名思义，客户端访问外部资源时，被无感知的进行流量代理。
    

- trans-proxy项目设计思路
  - 当[客户端]流量经过[代理服务器]时，借助iptables，将[客户端]的流量重定向到本地端口(代理服务程序)  
  - 由[代理服务程序]与[客户端]进行通信  
  - [代理服务程序]获取[客户端]要访问的[目的端]，由[代理服务程序]与[目的端]进行通信  
  - [代理服务程序]将[客户端]发送的请求数据重新发送给[目的端]  
  - [代理服务程序]将[目的端]发送的响应数据发送给[目的端]


- 技术思考
  - 整个通信过程都是通过Socket完成。根据一些高性能高并发的服务开发模式，使用异步非阻塞.
  - 非阻塞io + epoll事件驱动


- 数据结构

```
//代理socket
struct proxy_socket {
        int efd;     //事件监控句柄
        int type;    //socket的类型(监听，连接，传输)
        int from_fd; //数据来源socket
        int to_fd;   //数据接收socket
};
```


```
//socket类型
enum proxy_socket_type {
        PROXY_SOCKET_LISTEN,  //处于监听状态 
        PROXY_SOCKET_CONN,    //处理连接状态(未完成三次握手)
        PROXY_SOCKET_TRANS    //正数据传输状态
};
```


* 函数原型

`int server(short port)`:
创建一个监听Socket,  port为监听的端口
```
int ev_create()
int ev_add(int efd, int fd, void *ud)
void ev_write(int efd, int fd, void *ud, bool enable)
void ev_del(int efd, int fd)
int ev_wait(int efd, struct epoll_event *ev, int max)
int ev_close(int efd)
void ev_nonblocking(int fd)
```
以上是socket事件管理相关的函数，包括添加Socket, 监控Socket, 删除Socket，设置socket非阻塞等

```
void proxy_accept(struct proxy_socket *lis_socket)
void proxy_conn(struct proxy_socket *socket)
void proxy_trans(struct proxy_socket *socket)
```
三个事件处理函数，分别处理监听，连接，传输事件的socket



