/*
 * Copyright 2014-2025 Real Logic Limited.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#if defined(__linux__)
#define _BSD_SOURCE
#define _GNU_SOURCE
#endif

#include "util/aeron_platform.h"

#if defined(AERON_COMPILER_MSVC)
#include <io.h>
#else
#include <unistd.h>
#endif

#include "aeron_socket.h"

#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <inttypes.h>
#include "aeron_alloc.h"
#include "util/aeron_error.h"
#include "util/aeron_netutil.h"
#include "aeron_udp_channel_transport_xdp.h"
#include "aeron_udp_transport_poller.h"
#include "aeron_driver_context.h"

#include "xdp_socket.h"

#define AERON_XDP_FRAME_SIZE (4096)
#define AERON_XDP_QUEUE_DEFAULT (0)
#define AERON_XDP_QUEUE_LENGTH_DEFAULT (2048)
#define AERON_XDP_QUEUE_URI_PARAM_KEY "xdp-queue"

#if !defined(HAVE_STRUCT_MMSGHDR)
struct mmsghdr
{
    struct msghdr msg_hdr;
    unsigned int msg_len;
};
#endif

typedef struct aeron_udp_channel_transport_xdp_state_stct
{
    size_t so_rcvbuf;
    struct sockaddr_storage bound_addr;
}
aeron_udp_channel_transport_xdp_state_t;

static uint32_t aeron_udp_channel_transport_xdp_env_uint32(const char *name, uint32_t default_value)
{
    const char *value = getenv(name);
    return NULL != value ? (uint32_t)strtoul(value, NULL, 0) : default_value;
}

static int aeron_udp_channel_transport_xdp_resolve_local_ip(
    struct sockaddr_storage *bind_addr,
    struct sockaddr_storage *connect_addr,
    char *buffer,
    size_t length)
{
    struct sockaddr_in *bind_in = (struct sockaddr_in *)bind_addr;

    if (AF_INET == bind_addr->ss_family && INADDR_ANY != bind_in->sin_addr.s_addr)
    {
        return NULL == inet_ntop(AF_INET, &bind_in->sin_addr, buffer, length) ? -1 : 0;
    }

    if (NULL != connect_addr && AF_INET == connect_addr->ss_family)
    {
        int probe_fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (probe_fd < 0)
        {
            AERON_SET_ERR(errno, "%s", "failed to create probe socket for xdp local ip");
            return -1;
        }

        struct sockaddr_in local_addr;
        socklen_t local_addr_len = sizeof(local_addr);
        int result = -1;

        if (connect(probe_fd, (struct sockaddr *)connect_addr, sizeof(struct sockaddr_in)) == 0 &&
            getsockname(probe_fd, (struct sockaddr *)&local_addr, &local_addr_len) == 0)
        {
            result = NULL == inet_ntop(AF_INET, &local_addr.sin_addr, buffer, length) ? -1 : 0;
        }
        else
        {
            AERON_SET_ERR(errno, "%s", "failed to resolve xdp local ip from connect address");
        }

        aeron_close_socket(probe_fd);
        return result;
    }

    AERON_SET_ERR(EINVAL, "%s", "unable to determine local interface ip for xdp transport");
    return -1;
}

int aeron_udp_channel_transport_xdp_init(
    aeron_udp_channel_transport_t *transport,
    struct sockaddr_storage *bind_addr,
    struct sockaddr_storage *multicast_if_addr,
    struct sockaddr_storage *connect_addr,
    aeron_udp_channel_transport_params_t *params,
    aeron_driver_context_t *context,
    aeron_udp_channel_transport_affinity_t affinity)
{
    char iface_ip[AERON_NETUTIL_FORMATTED_MAX_LENGTH] = { 0 };
    aeron_udp_channel_transport_xdp_state_t *state = NULL;
    struct xdp_socket_config xdp_config;

    transport->fd = -1;
    transport->recv_fd = -1;
    transport->bindings_clientd = NULL;
    transport->timestamp_flags = AERON_UDP_CHANNEL_TRANSPORT_MEDIA_RCV_TIMESTAMP_NONE;
    transport->error_log = context->error_log;
    transport->errors_counter = aeron_system_counter_addr(context->system_counters, AERON_SYSTEM_COUNTER_ERRORS);
    for (size_t i = 0; i < AERON_UDP_CHANNEL_TRANSPORT_MAX_INTERCEPTORS; i++)
    {
        transport->interceptor_clientds[i] = NULL;
    }

    if (NULL == params)
    {
        AERON_SET_ERR(EINVAL, "%s", "channel transport params is NULL");
        goto error;
    }

    if (0 == params->mtu_length)
    {
        AERON_SET_ERR(EINVAL, "%s", "mtu_length must be greater than 0");
        goto error;
    }

    if (AF_INET != bind_addr->ss_family)
    {
        AERON_SET_ERR(EINVAL, "%s", "xdp transport supports IPv4 only");
        goto error;
    }

    if (aeron_is_addr_multicast(bind_addr))
    {
        AERON_SET_ERR(EINVAL, "%s", "xdp transport does not support multicast");
        goto error;
    }

    if (params->mtu_length > AERON_XDP_FRAME_SIZE)
    {
        AERON_SET_ERR(EINVAL, "mtu_length %" PRIu64 " exceeds xdp frame size", (uint64_t)params->mtu_length);
        goto error;
    }

    if (aeron_udp_channel_transport_xdp_resolve_local_ip(bind_addr, connect_addr, iface_ip, sizeof(iface_ip)) < 0)
    {
        AERON_APPEND_ERR("%s", "");
        goto error;
    }

    if (aeron_alloc((void **)&state, sizeof(aeron_udp_channel_transport_xdp_state_t)) < 0)
    {
        AERON_APPEND_ERR("%s", "failed to allocate xdp transport state");
        goto error;
    }
    transport->bindings_clientd = state;

    xdp_init_config(&xdp_config);
    xdp_config.iface_ip = iface_ip;
    xdp_config.queue = aeron_udp_channel_transport_xdp_env_uint32("AERON_XDP_QUEUE", AERON_XDP_QUEUE_DEFAULT);
    xdp_config.queue_length = aeron_udp_channel_transport_xdp_env_uint32(
        "AERON_XDP_QUEUE_LENGTH", AERON_XDP_QUEUE_LENGTH_DEFAULT);

    if (NULL != params->additional_params)
    {
        const char *queue_str = aeron_uri_find_param_value(params->additional_params, AERON_XDP_QUEUE_URI_PARAM_KEY);
        if (NULL != queue_str)
        {
            char *end_ptr = NULL;
            errno = 0;
            const long queue = strtol(queue_str, &end_ptr, 0);
            if (0 != errno || end_ptr == queue_str || '\0' != *end_ptr || queue < 0)
            {
                AERON_SET_ERR(EINVAL, "invalid %s: %s", AERON_XDP_QUEUE_URI_PARAM_KEY, queue_str);
                goto error;
            }
            xdp_config.queue = (uint32_t)queue;
        }
    }

    if ((transport->fd = xdp_socket(bind_addr->ss_family, SOCK_DGRAM, 0, &xdp_config)) < 0)
    {
        AERON_SET_ERR(EINVAL, "failed to create xdp socket on interface %s", iface_ip);
        goto error;
    }
    transport->recv_fd = transport->fd;

    memcpy(&state->bound_addr, bind_addr, sizeof(struct sockaddr_storage));
    struct sockaddr_in *bound_in = (struct sockaddr_in *)&state->bound_addr;
    inet_pton(AF_INET, iface_ip, &bound_in->sin_addr);
    if (0 == bound_in->sin_port && NULL != connect_addr)
    {
        bound_in->sin_port = ((struct sockaddr_in *)connect_addr)->sin_port;
    }

    if (xdp_bind(transport->fd, (struct sockaddr *)&state->bound_addr, sizeof(struct sockaddr_in)) < 0)
    {
        AERON_SET_ERR(EINVAL, "%s", "failed to xdp_bind transport");
        goto error;
    }

    if (NULL != connect_addr)
    {
        if (xdp_connect(transport->fd, (struct sockaddr *)connect_addr, AERON_ADDR_LEN(connect_addr)) < 0)
        {
            AERON_SET_ERR(EINVAL, "%s", "failed to xdp_connect transport");
            goto error;
        }
        transport->connected_address = connect_addr;
    }

    if (xdp_fcntl(transport->fd, F_SETFL, O_NONBLOCK) < 0)
    {
        AERON_SET_ERR(EINVAL, "%s", "failed to set xdp transport to be non-blocking");
        goto error;
    }

    state->so_rcvbuf = (size_t)xdp_config.queue_length * AERON_XDP_FRAME_SIZE;

    return 0;

error:
    aeron_udp_channel_transport_xdp_close(transport);
    return -1;
}

int aeron_udp_channel_transport_xdp_reconnect(
    aeron_udp_channel_transport_t *transport,
    struct sockaddr_storage *connect_addr)
{
    if (NULL != connect_addr && NULL != transport->connected_address)
    {
        if (xdp_connect(transport->fd, (struct sockaddr *)connect_addr, AERON_ADDR_LEN(connect_addr)) < 0)
        {
            AERON_SET_ERR(EINVAL, "%s", "failed to xdp_connect on reconnect");
            return -1;
        }

        transport->connected_address = connect_addr;
    }

    return 0;
}

int aeron_udp_channel_transport_xdp_close(aeron_udp_channel_transport_t *transport)
{
    if (transport->fd != -1)
    {
        xdp_close(transport->fd);
    }
    transport->recv_fd = -1;
    transport->fd = -1;

    aeron_free(transport->bindings_clientd);
    transport->bindings_clientd = NULL;

    return 0;
}

int aeron_udp_channel_transport_xdp_recvmmsg(
    aeron_udp_channel_transport_t *transport,
    struct mmsghdr *msgvec,
    size_t vlen,
    int64_t *bytes_rcved,
    aeron_udp_transport_recv_func_t recv_func,
    void *clientd)
{
    struct timespec tv = { .tv_nsec = 0, .tv_sec = 0 };

    int result = xdp_recvmmsg(transport->recv_fd, msgvec, (unsigned int)vlen, MSG_DONTWAIT, &tv);
    if (result < 0)
    {
        int err = errno;

        if (EINTR == err || EAGAIN == err || ECONNREFUSED == err)
        {
            return 0;
        }

        AERON_SET_ERR(err, "Failed to xdp_recvmmsg, fd=%d", transport->recv_fd);

        return -1;
    }
    else if (0 == result)
    {
        return 0;
    }
    else
    {
        for (size_t i = 0, length = (size_t)result; i < length; i++)
        {
            recv_func(
                transport->data_paths,
                transport,
                clientd,
                transport->dispatch_clientd,
                transport->destination_clientd,
                msgvec[i].msg_hdr.msg_iov[0].iov_base,
                msgvec[i].msg_len,
                msgvec[i].msg_hdr.msg_name,
                NULL);
            *bytes_rcved += msgvec[i].msg_len;
        }

        return result;
    }
}

int aeron_udp_channel_transport_xdp_send(
    aeron_udp_channel_data_paths_t *data_paths,
    aeron_udp_channel_transport_t *transport,
    struct sockaddr_storage *address,
    struct iovec *iov,
    size_t iov_length,
    int64_t *bytes_sent)
{
    struct mmsghdr msg[AERON_NETWORK_PUBLICATION_MAX_MESSAGES_PER_SEND];
    struct sockaddr_storage *dest = NULL != address ? address : transport->connected_address;
    size_t dest_len = NULL != dest ? AERON_ADDR_LEN(dest) : 0;
    size_t msg_i;

    for (msg_i = 0; msg_i < iov_length && msg_i < AERON_NETWORK_PUBLICATION_MAX_MESSAGES_PER_SEND; msg_i++)
    {
        msg[msg_i].msg_hdr.msg_control = NULL;
        msg[msg_i].msg_hdr.msg_controllen = 0;
        msg[msg_i].msg_hdr.msg_name = dest;
        msg[msg_i].msg_hdr.msg_namelen = dest_len;
        msg[msg_i].msg_hdr.msg_flags = 0;
        msg[msg_i].msg_hdr.msg_iov = &iov[msg_i];
        msg[msg_i].msg_hdr.msg_iovlen = 1;
        msg[msg_i].msg_len = 0;
    }

    int num_sent = xdp_sendmmsg(transport->fd, msg, (unsigned int)msg_i, 0);
    if (num_sent < 0)
    {
        if (EAGAIN == errno || EWOULDBLOCK == errno || ECONNREFUSED == errno || EINTR == errno)
        {
            return 0;
        }
        else
        {
            char addr_str[AERON_NETUTIL_FORMATTED_MAX_LENGTH];
            aeron_format_source_identity(addr_str, sizeof(addr_str), dest);
            AERON_SET_ERR(errno, "%s: address=%s", "failed to xdp_sendmmsg", addr_str);
            return -1;
        }
    }
    else
    {
        for (int i = 0; i < num_sent; i++)
        {
            *bytes_sent += msg[i].msg_len;
        }

        return num_sent;
    }
}

int aeron_udp_channel_transport_xdp_get_so_rcvbuf(aeron_udp_channel_transport_t *transport, size_t *so_rcvbuf)
{
    aeron_udp_channel_transport_xdp_state_t *state = transport->bindings_clientd;
    *so_rcvbuf = NULL != state ? state->so_rcvbuf : 0;

    return 0;
}

int aeron_udp_channel_transport_xdp_bind_addr_and_port(
    aeron_udp_channel_transport_t *transport, char *buffer, size_t length)
{
    aeron_udp_channel_transport_xdp_state_t *state = transport->bindings_clientd;

    if (NULL == state)
    {
        AERON_SET_ERR(EINVAL, "%s", "xdp transport not initialised");
        return -1;
    }

    return aeron_format_source_identity(buffer, length, &state->bound_addr);
}

aeron_udp_channel_transport_bindings_t aeron_udp_channel_transport_bindings_xdp =
    {
        aeron_udp_channel_transport_xdp_init,
        aeron_udp_channel_transport_xdp_reconnect,
        aeron_udp_channel_transport_xdp_close,
        aeron_udp_channel_transport_xdp_recvmmsg,
        aeron_udp_channel_transport_xdp_send,
        aeron_udp_channel_transport_xdp_get_so_rcvbuf,
        aeron_udp_channel_transport_xdp_bind_addr_and_port,
        aeron_udp_transport_poller_init,
        aeron_udp_transport_poller_close,
        aeron_udp_transport_poller_add,
        aeron_udp_transport_poller_remove,
        aeron_udp_transport_poller_poll,
        {
            "xdp",
            "media",
            NULL
        }
    };
