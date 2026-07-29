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

#ifndef AERON_UDP_CHANNEL_TRANSPORT_XDP_H
#define AERON_UDP_CHANNEL_TRANSPORT_XDP_H

#include "aeron_udp_channel_transport.h"
#include "aeron_udp_channel_transport_bindings.h"

extern aeron_udp_channel_transport_bindings_t aeron_udp_channel_transport_bindings_xdp;

int aeron_udp_channel_transport_xdp_init(
    aeron_udp_channel_transport_t *transport,
    struct sockaddr_storage *bind_addr,
    struct sockaddr_storage *multicast_if_addr,
    struct sockaddr_storage *connect_addr,
    aeron_udp_channel_transport_params_t *params,
    aeron_driver_context_t *context,
    aeron_udp_channel_transport_affinity_t affinity);

int aeron_udp_channel_transport_xdp_reconnect(
    aeron_udp_channel_transport_t *transport,
    struct sockaddr_storage *connect_addr);

int aeron_udp_channel_transport_xdp_close(aeron_udp_channel_transport_t *transport);

int aeron_udp_channel_transport_xdp_recvmmsg(
    aeron_udp_channel_transport_t *transport,
    struct mmsghdr *msgvec,
    size_t vlen,
    int64_t *bytes_rcved,
    aeron_udp_transport_recv_func_t recv_func,
    void *clientd);

int aeron_udp_channel_transport_xdp_send(
    aeron_udp_channel_data_paths_t *data_paths,
    aeron_udp_channel_transport_t *transport,
    struct sockaddr_storage *address,
    struct iovec *iov,
    size_t iov_length,
    int64_t *bytes_sent);

int aeron_udp_channel_transport_xdp_get_so_rcvbuf(aeron_udp_channel_transport_t *transport, size_t *so_rcvbuf);

int aeron_udp_channel_transport_xdp_bind_addr_and_port(
    aeron_udp_channel_transport_t *transport, char *buffer, size_t length);

#endif //AERON_UDP_CHANNEL_TRANSPORT_XDP_H
