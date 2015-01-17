#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <pthread.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ev.h>
#include "net.h"

#include "linklist.h"
#include "rbtree.h"

#include "server.h"
#include "mptunnel.h"
#include "buffer.h"
#include "client.h"


static struct ev_loop * g_ev_reactor = NULL;

static struct list_head g_buffers = LIST_HEAD_INIT(g_buffers);

static struct list_head g_bridge_list = LIST_HEAD_INIT(g_bridge_list);
static pthread_mutex_t g_bridge_list_mutex = PTHREAD_MUTEX_INITIALIZER;

static int g_listen_fd = -1;
static int g_target_fd = -1;

static int g_listen_port = 0;
static char *g_target_host = NULL;
static int g_target_port = 0;



/**
 * 收到远程桥发来的数据时的处理函数
 */
void recv_bridge_callback(struct ev_loop* reactor, ev_io* w, int events) {
    char* buf;
    int buflen = 65536;
    int readb;
    struct sockaddr_in *baddr;
    
    static received_t *received = NULL;
    if (received == NULL) {
        received = malloc(sizeof(*received));
        received_init(received);
    }
    
    buf = malloc(buflen);
    memset(buf, 0x00, buflen);
    
    bridge_t* b = (bridge_t*)malloc(sizeof(bridge_t));
    memset(b, 0x00, sizeof(*b));
    b->addrlen = sizeof(b->addr);
    baddr = (struct sockaddr_in*)&b->addr;
    
    LOGD("收到从 %d 发来的数据\n", w->fd);
    
    readb = recvfrom(w->fd, buf, buflen, 0, &b->addr, &b->addrlen);
    if (readb < 0) {
        LOGW("客户端可能断开了连接：%s\n", strerror(errno));
        free(buf); free(b);
        return;
    }
    else if (readb == 0) {
        LOGW("无法从客户端接收数据，客户端可能已经断开了连接\n");
        free(buf); free(b);
        return;
    }
    else {
        LOGD("从客户端(:%u)收取了 %d 字节数据：%s\n", htons(baddr->sin_port), readb, (char*)buf + sizeof(packet_t));
        
        int exists = 0;
        bridge_t *lb;
        struct list_head *l;
        pthread_mutex_lock(&g_bridge_list_mutex);
        
        list_for_each(l, &g_bridge_list) {
            lb = list_entry(l, bridge_t, list);
            if (memcmp(&lb->addr, &b->addr, sizeof(struct sockaddr)) == 0) {
                exists = 1;
                break;
            }
        }
        
        if (exists == 0) {
            list_add(&b->list, &g_bridge_list);
        }
        
        pthread_mutex_unlock(&g_bridge_list_mutex);
    }
    
    
    /// 解包，然后发送给目标服务器
    packet_t* p;
    p = (packet_t*)buf;
    if (p->type == PKT_TYPE_CTL) {
        LOGD("这是一个控制包，忽略\n");
        free(buf);
        return;
    }
    else if (p->type != PKT_TYPE_DATA) {
        LOGD("这不是一个未知类型的包，丢弃\n");
        free(buf);
        return;
    }
    
    buflen = p->buflen;
    buf = (char*)buf + sizeof(*p);
    
    if (received_is_received(received, p->id) == 1) {
        LOGD("编号为 %d 包已经发送过了，丢弃\n", p->id);
        free(p);
        
        received_destroy(received);
        free(received);
        
        return;
    }
    else {
        LOGD("向目标服务器转发编号为 %d 的包\n", p->id);
        received_add(received, p->id);
    }
    
    received_try_dropdead(received, 30);
    
    /// 发送给目标服务器
    int sendb;
    sendb = send(g_target_fd, buf, buflen, MSG_DONTWAIT);
    if (sendb < 0) {
        LOGW("无法向目标服务器发送数据：%s\n", strerror(errno));
    }
    else if (sendb == 0) {
        LOGW("目标服务器可能已经断开了连接\n");
    }
    else {
        LOGD("向目标服务器发送了 %d 字节数据：%s\n", buflen, buf);
    }
    
    free(p);
    return;
}


/**
 * ev 处理线程
 */
void* ev_thread(void* ptr) {
    LOGD("开始 EV 处理线程\n");
    
    g_listen_fd = net_bind("0.0.0.0", 3002, SOCK_DGRAM);
    if (g_listen_fd < 0) {
        LOGE("无法开始监听桥的请求: %s\n", strerror(errno));
        exit(0);
    }
    
    g_ev_reactor = ev_loop_new(EVFLAG_AUTO);
     
    ev_io* w = (ev_io*)malloc(sizeof(ev_io));
    ev_io_init(w, recv_bridge_callback, g_listen_fd, EV_READ);
    ev_io_start(g_ev_reactor, w);
     
    ev_run(g_ev_reactor, 0);
        
    LOGW("EV 处理线程退出\n");
}


/**
 * 向桥们发送数据
 */
int send_to_servers(char* buf, int buflen) {
    struct sockaddr* addr;
    struct sockaddr_in *baddr;
    socklen_t addrlen;
    int sendb;
    char ipstr[128] = {0};
    static int id = 0;
    
    packet_t* p;
    p = (packet_t*)malloc(sizeof(*p) + buflen);
    p->type = PKT_TYPE_DATA;
    p->id = ++id;
    p->buflen = buflen;
    memcpy(((char*)p) + sizeof(*p), buf, buflen);
    
    
    bridge_t *b;
    struct list_head *l;
    
    list_for_each(l, &g_bridge_list) {
        b = list_entry(l, bridge_t, list);
    
        sendb = sendto(g_listen_fd, p, buflen + sizeof(*p), 0, &b->addr, b->addrlen);
        if (sendb < 0) {
            LOGW("无法向桥(%s:%d)发送 %d 字节数据数据`%s', %s\n", ipstr, ntohs(baddr->sin_port), buflen, buf, strerror(errno));
        }
        else if (sendb == 0) {
            LOGW("无法向桥发送数据，桥可能已经断开\n");
        }
        else {
            LOGD("向桥发送了 %d 字节数据: %s\n", buflen + sizeof(*p), buf);
        }
    }
    
    free(p);
    
    return 0;
}




/**
 * 用于转发服务器消息到客户端的线程
 */
void* server_thread(void* ptr) {
    int readb, sendb, buflen;
    char* buf;
    
    LOGD("转发服务器消息到桥的线程启动了\n");
    
    
    buflen = 65536;
    buf = malloc(buflen);
    
    g_target_fd = net_connect(g_target_host, g_target_port, SOCK_DGRAM);
    if (g_target_fd < 0) {
        LOGE("无法创建到目标服务器的连接：%s\n", strerror(errno));
        return NULL;
    }
    
    while (1) {
        memset(buf, 0x00, sizeof(buflen));
        readb = recv(g_target_fd, buf, buflen, 0);
        if (readb < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            else {
                LOGI("目标服务器断开了连接: %s\n", strerror(errno));
                g_target_fd = net_connect(g_target_host, g_target_port, SOCK_DGRAM);
                continue;
            }
        }
        else if (readb == 0) {
            LOGW("无法从目标服务器收取消息，服务器断开了连接\n");
            g_target_fd = net_connect(g_target_host, g_target_port, SOCK_DGRAM);
            continue;
        }
        else {
            /// 收到了数据，将数据转发给桥
            send_to_servers(buf, readb);
        }
    }
    
    free(buf);
    
    LOGD("转发服务器消息到桥的线程退出了\n");
    
    return NULL;
}




int main(int argc, char** argv) {
    int clientfd, listenfd;
    
    if (argc <= 3) {
        fprintf(stderr, "Usage: <%s> <listen_port> <target_ip> <target_port>\n", argv[0]);
        exit(-1);
    }
    else {
        /// 载入配置信息
        g_listen_port = atoi(argv[1]);
        g_target_host = strdup(argv[2]);
        g_target_port = atoi(argv[3]);
        
        if (g_listen_port <= 0 || g_listen_port >= 65536) {
            LOGE("Invalid listen port `%s'\n", argv[1]);
            exit(-2);
        }
        if (g_target_port <= 0 || g_target_port >= 65536) {
            LOGE("Invalid target port `%s'\n", argv[3]);
            exit(-3);
        }
        
        LOGD("配置信息：本地监听端口：%d\n", g_listen_port);
        LOGD("配置信息：目标服务器：%s:%d\n", g_target_host, g_target_port);
    }
    
    
    
    LOGD("初始化 EV 处理线程\n");
    pthread_t tid;
    pthread_create(&tid, NULL, ev_thread, NULL);
    pthread_detach(tid);
    


    /// 创建转发数据到目标服务器的线程
    int* ptr = malloc(sizeof(int));
    *ptr = clientfd;
    pthread_create(&tid, NULL, server_thread, NULL);
    pthread_detach(tid);
    
    while (1) {
        sleep(100);
    }

    
    return 0;
}




/**
 * 初始化一个接收器 ev，用来处理收到的数据
 */
int init_recv_ev(int fd) {
    ev_io *watcher = (ev_io*)malloc(sizeof(ev_io));
    memset(watcher, 0x00, sizeof(*watcher));
    
    ev_io_init(watcher, recv_bridge_callback, fd, EV_READ);
    ev_io_start(g_ev_reactor, watcher);
    
    return 0;
}






