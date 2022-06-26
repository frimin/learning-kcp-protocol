#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "ikcp.h"

#define _MIN_(x, y) (((x) < (y)) ? (x) : (y))
#define PACKET_LIST_MAX 4096
//#define KCP_LOG
#define KCP_MTU 1400
#define KCP_MSS (KCP_MTU - 24)

#define KCP_WND 32, 128
#define KCP_NODELAY 0, 100, 0, 0 // nodelay, interval, resend, nc
#define KCP_THRESH_INIT 2
#define SEND_DATA_SIZE (KCP_MSS)
#define SEND_STEP 128
#define TIME_INCREMENT 100
#define K1_DROP_SN -1 
#define K2_DROP_SN -1
#define ACK_DELAY_FLUSH
#define RECV_TIME 0

static int k1_drop_arr[] = { K1_DROP_SN, -1 };
static int k2_drop_arr[] = { K2_DROP_SN, -1 };

static ikcpcb *k1 = NULL;
static ikcpcb *k2 = NULL;

/* 绑定到 KCP 对象上下文，存储了 output 的所有包 */
typedef struct OUTPUT_CONTEXT {
    ikcpcb **sendto;
    struct { char* buff; int sz; } list [PACKET_LIST_MAX];
    int sz;
    int* drop_arr;
    int drop_n;
    int send_n;
} output_context;

/* 调度 KCP 对象, 将缓存的数据包输入到目标 KCP 对象 */
static void dispatch_kcp(ikcpcb *kcp, IUINT32 current) {
    output_context *c = (output_context *)kcp->user;
    ikcp_update(kcp, current);
    if (c->sz != 0) {
        for (int i = 0; i < c->sz; i++) {
            int n = ikcp_input(*c->sendto, c->list[i].buff, c->list[i].sz);
            assert(n == 0);
            free(c->list[i].buff);
#ifndef ACK_DELAY_FLUSH
            if ((*c->sendto)->ackcount > 0) {
                ikcp_flush(*c->sendto);
            }
#endif
        }
        c->sz = 0;
#ifdef ACK_DELAY_FLUSH
        if ((*c->sendto)->ackcount > 0) {
            ikcp_flush(*c->sendto);
        }
#endif 
    }
}

/* KCP 回调函数，存储输出的数据包到缓存列表中
    真实的情况这里应该是执行网络发送 */
static int kcp_user_output(const char *buf, int len, ikcpcb *kcp, void *user) {
    output_context *c = (output_context *)user;

    c->send_n++;

    IUINT32 sn = *(IUINT32*)(buf + 12);

    if (c->drop_arr[c->drop_n] != -1 && sn == c->drop_arr[c->drop_n]) {
        if (kcp->writelog != NULL) {
            char logbuff[128];
            sprintf(logbuff, "drop sn=%d", sn);
            kcp->writelog(logbuff, kcp, user);
        }
        c->drop_n++;
        return 0;
    }

    if (c->sz >= PACKET_LIST_MAX)
        return 0;

    char *newbuf = malloc(len);
    memcpy(newbuf, buf, len);
    c->list[c->sz].buff = newbuf;
    c->list[c->sz].sz = len;
    c->sz++;
    return 0;
}

#ifdef KCP_LOG
static void kcp_user_writelog(const char *log, ikcpcb *kcp, void *user) {
    printf("t=%d %s %s\n", kcp->current, ((output_context*)user)->sendto == &k1 ? "k2" : "k1", log);
}
#endif

/* 初始化其它相关数据 */
static void setup_kcp(ikcpcb *kcp, ikcpcb **sendto, int* drop_arr) {
    ikcp_nodelay(kcp, KCP_NODELAY);
    ikcp_wndsize(kcp, KCP_WND);
    output_context *context = malloc(sizeof(output_context));
    memset(context, 0, sizeof(output_context));
    context->sendto = sendto;
    context->drop_arr = drop_arr;
    kcp->user = context;
    kcp->output = kcp_user_output;
#ifdef KCP_LOG
    kcp->writelog = kcp_user_writelog;
#endif
    kcp->logmask = ~IKCP_LOG_RECV;
    /* 必须调用一次 ikcp_update 以完成初始化 */
    ikcp_update(kcp, 0);
}

static int get_cwnd(ikcpcb *kcp) {
    int cwnd;
    cwnd = _MIN_(kcp->snd_wnd, kcp->rmt_wnd);
	if (kcp->nocwnd == 0) cwnd = _MIN_(kcp->cwnd, cwnd);
    return cwnd;
}

int main(int argc, char *argv[]) {
    int n; 
    int send_n = 0;
    int read_n = 0;
    char send_buff[SEND_DATA_SIZE];
    char recv_buff[SEND_DATA_SIZE];

    for (int i = 0; i < SEND_DATA_SIZE; i++)
        send_buff[i] = i % UINT8_MAX;

    k1 = ikcp_create(1, NULL); setup_kcp(k1, &k2, k1_drop_arr);
    k2 = ikcp_create(1, NULL); setup_kcp(k2, &k1, k2_drop_arr);

    k1->ssthresh = KCP_THRESH_INIT; 

    IUINT32 current = 0;

    output_context *k1_ctx = (output_context*)k1->user;

    for (;;) {
        if (send_n < SEND_STEP) {
            send_n++;
            n = ikcp_send(k1, send_buff, SEND_DATA_SIZE);
            ikcp_flush(k1);
            assert(n == 0);
        }

        dispatch_kcp(k1, current); 

        printf("t=%d n=%d una=%d nxt=%d cwnd=%d|%d ssthresh=%d incr=%d\n", 
            k1->current, 
            k1_ctx->send_n,
            k1->snd_una, 
            k1->snd_nxt, 
            get_cwnd(k1), 
            k1->cwnd, 
            k1->ssthresh, 
            k1->incr);

        //printf("%d\t%d\n", k1->incr, k1->current);

        k1_ctx->send_n = 0;

        dispatch_kcp(k2, current);

        if (current >= RECV_TIME)
            n = ikcp_recv(k2, recv_buff, SEND_DATA_SIZE);

        if (n == SEND_DATA_SIZE) {
            read_n++;
            assert(memcmp(send_buff, recv_buff, SEND_DATA_SIZE) == 0);
        }

        if (read_n == SEND_STEP)
            break;

        current += TIME_INCREMENT;
    }

    free(k1->user); ikcp_release(k1);
    free(k2->user); ikcp_release(k2);
    return 0;
}