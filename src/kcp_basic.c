#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "ikcp.h"

#define DATA_SIZE 4096
#define LIST_MAX 4

static ikcpcb *k1 = NULL;
static ikcpcb *k2 = NULL;

/* 绑定到 KCP 对象上下文，存储了 output 的所有包 */
typedef struct OUTPUT_CONTEXT {
    ikcpcb **sendto;
    struct { char* buff; int sz; } list [LIST_MAX];
    int sz;
} output_context;

/* 调度 KCP 对象, 将缓存的数据包输入到目标 KCP 对象 */
static void dispatch_kcp(ikcpcb *kcp) {
    output_context *c = (output_context *)kcp->user;
    if (c->sz == 0)
        return;
    for (int i = 0; i < c->sz; i++) {
        int n = ikcp_input(*c->sendto, c->list[i].buff, c->list[i].sz);
        assert(n == 0);
        free(c->list[i].buff);
    }
    /* 可能发回确认包 */
    ikcp_flush(*c->sendto);
    c->sz = 0;
}

/* KCP 回调函数，存储输出的数据包到缓存列表中
    真实的情况这里应该是执行网络发送 */
static int kcp_user_output(const char *buf, int len, ikcpcb *kcp, void *user) {
    output_context *c = (output_context *)user;
    assert(c->sz < LIST_MAX);
    char *newbuf = malloc(len);
    memcpy(newbuf, buf, len);
    c->list[c->sz].buff = newbuf;
    c->list[c->sz].sz = len;
    c->sz++;
    return 0;
}

static void kcp_user_writelog(const char *log, ikcpcb *kcp, void *user) {
    printf("%s %s\n", ((output_context*)user)->sendto == &k1 ? "k2" : "k1", log);
}

/* 初始化其它相关数据 */
static void setup_kcp(ikcpcb *kcp, ikcpcb **sendto) {
    output_context *context = malloc(sizeof(output_context));
    memset(context, 0, sizeof(output_context));
    context->sendto = sendto;
    kcp->user = context;
    kcp->output = kcp_user_output;
    kcp->writelog = kcp_user_writelog;
    kcp->logmask = ~0;
    /* 必须调用一次 ikcp_update 以完成初始化 */
    ikcp_update(kcp, 0);
}

int main(int argc, char *argv[]) {
    int n;
    char send_buff[DATA_SIZE];
    char recv_buff[DATA_SIZE];

    /* 填充一些发送数据 */
    for (int i = 0; i < DATA_SIZE; i++)
        send_buff[i] = i % UINT8_MAX;

    /* 初始化 KCP 对象 */
    k1 = ikcp_create(1, NULL); setup_kcp(k1, &k2);
    k2 = ikcp_create(1, NULL); setup_kcp(k2, &k1);

    /* 发送数据 */
    n = ikcp_send(k1, send_buff, DATA_SIZE);
    /* 立即刷出数据，执行 output 回调 */
    ikcp_flush(k1);
    
    assert(n == 0);

    for (;;) {
        dispatch_kcp(k1); /* 调度 k1 的输出包输入到 k2 中 */
        dispatch_kcp(k2); /* 调度 k2 的输出包输入到 k1 中 */
        /* 尝试读出数据 */
        n = ikcp_recv(k2, recv_buff, DATA_SIZE);
        if (n == DATA_SIZE)
            break;
    }

    assert(memcmp(send_buff, recv_buff, DATA_SIZE) == 0);
    free(k1->user); ikcp_release(k1);
    free(k2->user); ikcp_release(k2);
    return 0;
}