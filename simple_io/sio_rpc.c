#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include "shash.h"
#include "sio.h"
#include "sio_stream.h"
#include "sio_rpc.h"

static void _sio_rpc_free_call(struct sio_rpc_request *req);
static void _sio_rpc_reset_upstream(struct sio_rpc_upstream *upstream);
static void _sio_rpc_remove_upstream(struct sio_rpc_client *client, struct sio_rpc_upstream *upstream);

struct sio_rpc *sio_rpc_new(struct sio *sio)
{
    struct sio_rpc *rpc = malloc(sizeof(*rpc));
    rpc->sio = sio;
    return rpc;
}

void sio_rpc_free(struct sio_rpc *rpc)
{
    free(rpc);
}

struct sio_rpc_client *sio_rpc_client_new(struct sio_rpc *rpc)
{
    struct sio_rpc_client *client = malloc(sizeof(*client));
    client->rpc = rpc;
    client->rr_stream = 0;
    client->upstream_count = 0;
    client->upstreams = NULL;
    assert(client->req_record = shash_new()); /* 所有运行中的rpc request地址都记录, 以便client_free时回收  */
    return client;
}

void sio_rpc_client_free(struct sio_rpc_client *client)
{
    int i;
    for (i = 0; i < client->upstream_count; ++i)
        _sio_rpc_remove_upstream(client, client->upstreams[i]);

    shash_begin_iterate(client->req_record);
    const char *key;
    while (shash_iterate(client->req_record, &key, NULL, NULL) != -1) {
        struct sio_rpc_request *req = *(struct sio_rpc_request * const*)key;
        sio_stop_timer(client->rpc->sio, &req->timer);
        /* XXX: 回调用户, 通知请求超时, 用户必须保证不再发起更多的call, 否则死循环. */
        req->cb(client, 1, NULL, 0, req->arg);
        _sio_rpc_free_call(req);
    }
    shash_end_iterate(client->req_record);

    shash_free(client->req_record);
    free(client->upstreams);
    free(client);
}

static int _sio_rpc_upstream_parse_response(struct sio *sio, struct sio_rpc_upstream *upstream)
{
    struct sio_stream *stream = upstream->stream;

    struct sio_buffer *input = sio_stream_buffer(stream);
    uint64_t size;
    char *data= sio_buffer_data(input, &size);

    uint64_t used = 0;
    while (used < size) {
        uint64_t left = size - used;
        if (left < SHEAD_ENCODE_SIZE)
            break; /* header不完整 */
        struct shead head;
        if (shead_decode(&head, data + used, SHEAD_ENCODE_SIZE) == -1)
            return -1; /* header不合法 */
        if (left - SHEAD_ENCODE_SIZE < head.body_len)
            break; /* body不完整 */
        void *value;
        if (shash_find(upstream->req_status, (const char *)&head.id, sizeof(head.id), &value) == 0) { /* 找到call */
            assert(shash_erase(upstream->req_status, (const char *)&head.id, sizeof(head.id)) == 0);
            struct sio_rpc_request *req = value;
            if (head.type == req->type) { /* call的type相同, 回调用户, 关闭超时定时器, 释放call */
                req->cb(upstream->client, 0, data + used + SHEAD_ENCODE_SIZE, head.body_len, req->arg);
                sio_stop_timer(sio, &req->timer);
                _sio_rpc_free_call(req);
            } else { /* 请求与应答的type不同, 客户端有bug才会至此, 作为超时处理 */
                req->upstream = NULL;
            }
        } /* 没有找到对应的call, 忽略此应答 */
        used += SHEAD_ENCODE_SIZE + head.body_len;
    }
    sio_buffer_erase(input, used);
    return 0;
}

static void _sio_rpc_upstream_callback(struct sio *sio, struct sio_stream *stream, enum sio_stream_event event, void *arg)
{
    struct sio_rpc_upstream *upstream = arg;
    int err = 0;
    switch (event) {
    case SIO_STREAM_DATA:
        err = _sio_rpc_upstream_parse_response(sio, upstream);
        break;
    case SIO_STREAM_ERROR:
    case SIO_STREAM_CLOSE:
        _sio_rpc_reset_upstream(upstream);
        break;
    default:
        assert(0);
    }
}

static void _sio_rpc_upstream_timer(struct sio *sio, struct sio_timer *timer, void *arg)
{
    struct sio_rpc_upstream *upstream = arg;

    /* TODO: 检查stream的pending长度/pending请求数, 过长则断开连接 */

    /* 当前只检查连接状态, 并发起重连 */
    if (!upstream->stream)
        upstream->stream = sio_stream_connect(sio, upstream->ip, upstream->port, _sio_rpc_upstream_callback, upstream);

    sio_start_timer(upstream->client->rpc->sio, &upstream->timer, 1000, _sio_rpc_upstream_timer, upstream);
}

void sio_rpc_add_upstream(struct sio_rpc_client *client, const char *ip, uint16_t port)
{
    uint32_t i;
    for (i = 0; i < client->upstream_count; ++i) {
        if (strcmp(client->upstreams[i]->ip, ip) == 0 && client->upstreams[i]->port == port)
            return;
    }

    struct sio_rpc_upstream *upstream = malloc(sizeof(*upstream));
    upstream->ip = strdup(ip);
    upstream->port = port;
    upstream->req_id = 0;
    upstream->client = client;
    upstream->req_status = shash_new();
    upstream->stream = sio_stream_connect(client->rpc->sio, ip, port, _sio_rpc_upstream_callback, upstream);

    sio_start_timer(client->rpc->sio, &upstream->timer, 1000, _sio_rpc_upstream_timer, upstream);

    client->upstreams = realloc(client->upstreams, ++client->upstream_count * sizeof(*client->upstreams));
    client->upstreams[client->upstream_count - 1] = upstream;
}

static void _sio_rpc_remove_upstream(struct sio_rpc_client *client, struct sio_rpc_upstream *upstream)
{
    if (upstream->stream) /* 关闭连接, 重置所有排队请求 */
        _sio_rpc_reset_upstream(upstream);
    sio_stop_timer(client->rpc->sio, &upstream->timer); /* 关闭定时器 */
    free(upstream->ip);
    shash_free(upstream->req_status);
    free(upstream);
}

void sio_rpc_remove_upstream(struct sio_rpc_client *client, const char *ip, uint16_t port)
{
    struct sio_rpc_upstream *upstream = NULL;

    uint32_t i;
    for (i = 0; i < client->upstream_count; ++i) {
        if (strcmp(client->upstreams[i]->ip, ip) == 0 && client->upstreams[i]->port == port) {
            upstream = client->upstreams[i];
            break;
        }
    }
    if (!upstream)
        return;
    if (i != client->upstream_count - 1)
        client->upstreams[i] = client->upstreams[client->upstream_count - 1]; /* 最后一个upstream往前放 */
    client->upstream_count--;
    _sio_rpc_remove_upstream(client, upstream);
}

static struct sio_rpc_upstream *_sio_rpc_choose_upstream(struct sio_rpc_client *client)
{
    uint64_t min_pending = ~0;
    struct sio_rpc_upstream *upstream = NULL;

    uint32_t i;
    for (i = 0; i < client->upstream_count; ++i) {
        if (client->upstreams[i]->stream) {
            uint64_t pending = shash_size(client->upstreams[i]->req_status);
            if (pending <= min_pending) {
                min_pending = pending;
                upstream = client->upstreams[i];
            }
        }
    }
    if (upstream)
        return upstream;
    upstream = client->upstreams[client->rr_stream++ % client->upstream_count];
    if (!(upstream->stream = sio_stream_connect(client->rpc->sio, upstream->ip, upstream->port, _sio_rpc_upstream_callback, upstream)))
        return NULL;
    return upstream;
}

static void _sio_rpc_free_call(struct sio_rpc_request *req)
{
    assert(shash_erase(req->client->req_record, (const char *)&req, sizeof(req)) == 0);
    free(req->body);
    free(req);
}

static int _sio_rpc_call(struct sio_rpc_upstream *upstream, struct sio_rpc_request *req);

/* rpc call 超时 */
static void _sio_rpc_call_timer(struct sio *sio, struct sio_timer *timer, void *arg)
{
    struct sio_rpc_request *req = arg;

    if (req->upstream) { /* call已送出, 取消call */
        assert(shash_erase(req->upstream->req_status, (const char *)&req->id, sizeof(req->id)) == 0);
        req->upstream = NULL;
    }
    /* call超过重试限制, 回调用户 */
    if (req->retry_count++ >= req->retry_times) {
        req->cb(req->client, 1, NULL, 0, req->arg);
        _sio_rpc_free_call(req);
    } else { /* 重新选择upstream, 发起call重试 */
        sio_start_timer(req->client->rpc->sio, &req->timer, req->timeout, _sio_rpc_call_timer, req);
        req->upstream = _sio_rpc_choose_upstream(req->client);
        if (req->upstream && _sio_rpc_call(req->upstream, req) == -1)
            req->upstream = NULL; /* 重试call失败, 等待请求超时 */
    }
}

static void _sio_rpc_reset_upstream(struct sio_rpc_upstream *upstream)
{
    /* 关闭连接, 等待定时器检测发起重连 */
    struct sio *sio = upstream->client->rpc->sio;
    sio_stream_close(sio, upstream->stream);
    upstream->stream = NULL;
    upstream->req_id = 0;

    /* 重置所有等待应答的请求的upstream为NULL, 等待超时后重新调度到其他upstream */
    shash_begin_iterate(upstream->req_status);
    const char *key;
    void *value;
    while (shash_iterate(upstream->req_status, &key, NULL, &value) != -1) {
        struct sio_rpc_request *req = value;
        req->upstream = NULL;
        assert(shash_erase(upstream->req_status, (const char *)&req->id, sizeof(req->id)) == 0);
    }
    shash_end_iterate(upstream->req_status);
}

static int _sio_rpc_call(struct sio_rpc_upstream *upstream, struct sio_rpc_request *req)
{
    struct sio *sio = upstream->client->rpc->sio;
    struct sio_stream *stream = upstream->stream;

    req->id = upstream->req_id++;

    struct shead shead;
    shead.id = req->id;
    shead.type = req->type;
    shead.reserved = 0;
    shead.body_len = req->bodylen;

    char head[SHEAD_ENCODE_SIZE];
    assert(shead_encode(&shead, head, sizeof(head)) == 0);

    int sent_head = sio_stream_write(sio, stream, head, sizeof(head));
    int sent_body = sio_stream_write(sio, stream, req->body, req->bodylen);
    if (sent_head == 0 && sent_body == 0) { /* call成功发出, 记录状态, 等待应答或者超时 */
        assert(shash_insert(upstream->req_status, (const char *)&req->id, sizeof(req->id), req) == 0);
        return 0;
    }
    /* call发送失败, 重置upstream, 等待超时 */
    _sio_rpc_reset_upstream(upstream);
    return -1;
}

/* 发起远程调用, 消息类型type, 请求超时timeout_ms, 重试次数retry_times, 请求request, 请求长度size, 结果回调cb, 回调参数arg */
void sio_rpc_call(struct sio_rpc_client *client, uint32_t type, uint64_t timeout_ms, uint32_t retry_times,
        const char *request, uint32_t size, sio_rpc_upstream_callback_t cb, void *arg)
{
    struct sio_rpc_upstream *upstream = _sio_rpc_choose_upstream(client);

    struct sio_rpc_request *req = malloc(sizeof(*req));
    req->timeout = timeout_ms;
    req->retry_times = retry_times;
    req->retry_count = 0;
    req->type = type;
    req->body = malloc(size);
    req->bodylen = size;
    memcpy(req->body, request, size);
    req->cb = cb;
    req->arg = arg;
    req->client = client;
    req->upstream = upstream;
    sio_start_timer(client->rpc->sio, &req->timer, timeout_ms, _sio_rpc_call_timer, req);
    assert(shash_insert(client->req_record, (const char *)&req, sizeof(req), NULL) == 0);

    if (!req->upstream)
        return; /* 无可用连接, 等待请求超时 */
    if (_sio_rpc_call(upstream, req) == -1)
        req->upstream = NULL; /* call失败, 等待请求超时 */
}
