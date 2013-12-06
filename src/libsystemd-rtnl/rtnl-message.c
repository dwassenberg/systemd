/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright 2013 Tom Gundersen <teg@jklm.no>

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include <linux/rtnetlink.h>
#include <netinet/in.h>
#include <netinet/ether.h>
#include <stdbool.h>
#include <unistd.h>

#include "util.h"
#include "refcnt.h"

#include "sd-rtnl.h"
#include "rtnl-internal.h"

struct sd_rtnl_message {
        RefCount n_ref;

        struct nlmsghdr *hdr;

        struct rtattr *current_container;

        struct rtattr *next_rta;
        size_t remaining_size;

        bool sealed:1;
};

static int message_new(sd_rtnl_message **ret, size_t initial_size) {
        sd_rtnl_message *m;

        assert_return(ret, -EINVAL);
        assert_return(initial_size >= sizeof(struct nlmsghdr), -EINVAL);

        m = new0(sd_rtnl_message, 1);
        if (!m)
                return -ENOMEM;

        m->hdr = malloc0(initial_size);
        if (!m->hdr) {
                free(m);
                return -ENOMEM;
        }

        m->n_ref = REFCNT_INIT;

        m->hdr->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
        m->sealed = false;

        *ret = m;

        return 0;
}

int message_new_synthetic_error(int error, uint32_t serial, sd_rtnl_message **ret) {
        struct nlmsgerr *err;
        int r;

        assert(error <= 0);

        r = message_new(ret, NLMSG_SPACE(sizeof(struct nlmsgerr)));
        if (r < 0)
                return r;

        (*ret)->hdr->nlmsg_len = NLMSG_LENGTH(sizeof(struct nlmsgerr));
        (*ret)->hdr->nlmsg_type = NLMSG_ERROR;
        (*ret)->hdr->nlmsg_seq = serial;

        err = NLMSG_DATA((*ret)->hdr);

        err->error = error;

        return 0;
}

bool message_type_is_route(uint16_t type) {
        switch (type) {
                case RTM_NEWROUTE:
                case RTM_GETROUTE:
                case RTM_DELROUTE:
                        return true;
                default:
                        return false;
        }
}

bool message_type_is_link(uint16_t type) {
        switch (type) {
                case RTM_NEWLINK:
                case RTM_SETLINK:
                case RTM_GETLINK:
                case RTM_DELLINK:
                        return true;
                default:
                        return false;
        }
}

bool message_type_is_addr(uint16_t type) {
        switch (type) {
                case RTM_NEWADDR:
                case RTM_GETADDR:
                case RTM_DELADDR:
                        return true;
                default:
                        return false;
        }
}

int sd_rtnl_message_route_new(uint16_t nlmsg_type, unsigned char rtm_family,
                              unsigned char rtm_dst_len, unsigned char rtm_src_len,
                              unsigned char rtm_tos, unsigned char rtm_table,
                              unsigned char rtm_scope, unsigned char rtm_protocol,
                              unsigned char rtm_type, unsigned rtm_flags, sd_rtnl_message **ret) {
        struct rtmsg *rtm;
        int r;

        assert_return(message_type_is_route(nlmsg_type), -EINVAL);
        assert_return(ret, -EINVAL);

        r = message_new(ret, NLMSG_SPACE(sizeof(struct rtmsg)));
        if (r < 0)
                return r;

        (*ret)->hdr->nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
        (*ret)->hdr->nlmsg_type = nlmsg_type;
        if (nlmsg_type == RTM_NEWROUTE)
                (*ret)->hdr->nlmsg_flags |= NLM_F_CREATE | NLM_F_EXCL;

        rtm = NLMSG_DATA((*ret)->hdr);

        rtm->rtm_family = rtm_family;
        rtm->rtm_dst_len = rtm_dst_len;
        rtm->rtm_src_len = rtm_src_len;
        rtm->rtm_tos = rtm_tos;
        rtm->rtm_table = rtm_table;
        rtm->rtm_protocol = rtm_protocol;
        rtm->rtm_scope = rtm_scope;
        rtm->rtm_type = rtm_type;
        rtm->rtm_flags = rtm_flags;

        return 0;
}

int sd_rtnl_message_link_set_flags(sd_rtnl_message *m, unsigned flags) {
        struct ifinfomsg *ifi;

        ifi = NLMSG_DATA(m->hdr);

        ifi->ifi_flags = flags;

        return 0;
}

int sd_rtnl_message_link_set_type(sd_rtnl_message *m, unsigned type) {
        struct ifinfomsg *ifi;

        ifi = NLMSG_DATA(m->hdr);

        ifi->ifi_type = type;

        return 0;
}

int sd_rtnl_message_link_new(uint16_t nlmsg_type, int index, sd_rtnl_message **ret) {
        struct ifinfomsg *ifi;
        int r;

        assert_return(message_type_is_link(nlmsg_type), -EINVAL);
        assert_return(nlmsg_type == RTM_NEWLINK || index > 0, -EINVAL);
        assert_return(ret, -EINVAL);

        r = message_new(ret, NLMSG_SPACE(sizeof(struct ifinfomsg)));
        if (r < 0)
                return r;

        (*ret)->hdr->nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
        (*ret)->hdr->nlmsg_type = nlmsg_type;
        if (nlmsg_type == RTM_NEWLINK)
                (*ret)->hdr->nlmsg_flags |= NLM_F_CREATE;

        ifi = NLMSG_DATA((*ret)->hdr);

        ifi->ifi_family = AF_UNSPEC;
        ifi->ifi_index = index;
        ifi->ifi_change = 0xffffffff;

        return 0;
}

int sd_rtnl_message_addr_new(uint16_t nlmsg_type, int index, unsigned char family, unsigned char prefixlen, unsigned char flags, unsigned char scope, sd_rtnl_message **ret) {
        struct ifaddrmsg *ifa;
        int r;

        assert_return(message_type_is_addr(nlmsg_type), -EINVAL);
        assert_return(index > 0, -EINVAL);
        assert_return(ret, -EINVAL);

        r = message_new(ret, NLMSG_SPACE(sizeof(struct ifaddrmsg)));
        if (r < 0)
                return r;

        (*ret)->hdr->nlmsg_len = NLMSG_LENGTH(sizeof(struct ifaddrmsg));
        (*ret)->hdr->nlmsg_type = nlmsg_type;

        ifa = NLMSG_DATA((*ret)->hdr);

        ifa->ifa_family = family;
        ifa->ifa_prefixlen = prefixlen;
        ifa->ifa_flags = flags;
        ifa->ifa_scope = scope;
        ifa->ifa_index = index;

        return 0;
}

sd_rtnl_message *sd_rtnl_message_ref(sd_rtnl_message *m) {
        if (m)
                assert_se(REFCNT_INC(m->n_ref) >= 2);

        return m;
}

sd_rtnl_message *sd_rtnl_message_unref(sd_rtnl_message *m) {
        if (m && REFCNT_DEC(m->n_ref) <= 0) {
                free(m->hdr);
                free(m);
        }

        return NULL;
}

int sd_rtnl_message_get_type(sd_rtnl_message *m, uint16_t *type) {
        assert_return(m, -EINVAL);
        assert_return(type, -EINVAL);

        *type = m->hdr->nlmsg_type;

        return 0;
}

int sd_rtnl_message_link_get_ifindex(sd_rtnl_message *m, int *ifindex) {
        struct ifinfomsg *ifi;

        assert_return(m, -EINVAL);
        assert_return(m->hdr, -EINVAL);
        assert_return(message_type_is_link(m->hdr->nlmsg_type), -EINVAL);
        assert_return(ifindex, -EINVAL);

        ifi = NLMSG_DATA(m->hdr);

        *ifindex = ifi->ifi_index;

        return 0;
}

int sd_rtnl_message_link_get_flags(sd_rtnl_message *m, unsigned *flags) {
        struct ifinfomsg *ifi;

        assert_return(m, -EINVAL);
        assert_return(m->hdr, -EINVAL);
        assert_return(message_type_is_link(m->hdr->nlmsg_type), -EINVAL);
        assert_return(flags, -EINVAL);

        ifi = NLMSG_DATA(m->hdr);

        *flags = ifi->ifi_flags;

        return 0;
}

/* If successful the updated message will be correctly aligned, if unsuccessful the old message is
   untouched */
static int add_rtattr(sd_rtnl_message *m, unsigned short type, const void *data, size_t data_length) {
        uint32_t rta_length, message_length;
        struct nlmsghdr *new_hdr;
        struct rtattr *rta;
        char *padding;

        assert(m);
        assert(m->hdr);
        assert(NLMSG_ALIGN(m->hdr->nlmsg_len) == m->hdr->nlmsg_len);
        assert(!data || data_length > 0);

        /* get the size of the new rta attribute (with padding at the end) */
        rta_length = RTA_LENGTH(data_length);
        /* get the new message size (with padding at the end)
         */
        message_length = m->hdr->nlmsg_len + RTA_ALIGN(rta_length);

        /* realloc to fit the new attribute */
        new_hdr = realloc(m->hdr, message_length);
        if (!new_hdr)
                return -ENOMEM;
        m->hdr = new_hdr;

        /* get pointer to the attribute we are about to add */
        rta = (struct rtattr *) ((uint8_t *) m->hdr + m->hdr->nlmsg_len);
        /* update message size */
        m->hdr->nlmsg_len = message_length;

        /* we are inside a container, extend it */
        if (m->current_container)
                m->current_container->rta_len = (unsigned char *) m->hdr +
                                                m->hdr->nlmsg_len -
                                                (unsigned char *) m->current_container;

        /* fill in the attribute */
        rta->rta_type = type;
        rta->rta_len = rta_length;
        if (!data) {
                /* this is a container, set pointer */
                m->current_container = rta;
        } else {
                /* we don't deal with the case where the user lies about the type
                 * and gives us too little data (so don't do that)
                */
                padding = mempcpy(RTA_DATA(rta), data, data_length);
                /* make sure also the padding at the end of the message is initialized */
                memset(padding, '\0', (unsigned char *) m->hdr +
                                      m->hdr->nlmsg_len -
                                      (unsigned char *) padding);
        }

        return 0;
}

int sd_rtnl_message_append(sd_rtnl_message *m, unsigned short type, const void *data) {
        uint16_t rtm_type;
        struct ifaddrmsg *ifa;
        struct rtmsg *rtm;

        assert_return(m, -EINVAL);
        assert_return(data, -EINVAL);

        sd_rtnl_message_get_type(m, &rtm_type);

        if (m->current_container) {
                switch (rtm_type) {
                        case RTM_NEWLINK:
                        case RTM_SETLINK:
                        case RTM_GETLINK:
                        case RTM_DELLINK:
                                switch (m->current_container->rta_type) {
                                        case IFLA_LINKINFO:
                                                switch (type) {
                                                        case IFLA_INFO_KIND:
                                                                return add_rtattr(m, type, data, strlen(data) + 1);
                                                        default:
                                                                return -ENOTSUP;
                                                }
                                        default:
                                                return -ENOTSUP;
                                }
                        default:
                                return -ENOTSUP;
                }
        }

        switch (rtm_type) {
                case RTM_NEWLINK:
                case RTM_SETLINK:
                case RTM_DELLINK:
                case RTM_GETLINK:
                        switch (type) {
                                case IFLA_IFNAME:
                                case IFLA_IFALIAS:
                                case IFLA_QDISC:
                                        return add_rtattr(m, type, data, strlen(data) + 1);
                                case IFLA_MASTER:
                                case IFLA_MTU:
                                case IFLA_LINK:
                                        return add_rtattr(m, type, data, sizeof(uint32_t));
                                case IFLA_STATS:
                                        return add_rtattr(m, type, data, sizeof(struct rtnl_link_stats));
                                case IFLA_ADDRESS:
                                case IFLA_BROADCAST:
                                        return add_rtattr(m, type, data, ETH_ALEN);
                                default:
                                        return -ENOTSUP;
                        }
                case RTM_NEWADDR:
                case RTM_DELADDR:
                case RTM_GETADDR:
                        switch (type) {
                                case IFA_LABEL:
                                        return add_rtattr(m, type, data, strlen(data) + 1);
                                case IFA_ADDRESS:
                                case IFA_LOCAL:
                                case IFA_BROADCAST:
                                case IFA_ANYCAST:
                                        ifa = NLMSG_DATA(m->hdr);
                                        switch (ifa->ifa_family) {
                                                case AF_INET:
                                                        return add_rtattr(m, type, data, sizeof(struct in_addr));
                                                case AF_INET6:
                                                        return add_rtattr(m, type, data, sizeof(struct in6_addr));
                                                default:
                                                        return -EINVAL;
                                        }
                                default:
                                        return -ENOTSUP;
                        }
                case RTM_NEWROUTE:
                case RTM_DELROUTE:
                case RTM_GETROUTE:
                        switch (type) {
                                case RTA_DST:
                                case RTA_SRC:
                                case RTA_GATEWAY:
                                        rtm = NLMSG_DATA(m->hdr);
                                        switch (rtm->rtm_family) {
                                                case AF_INET:
                                                        return add_rtattr(m, type, data, sizeof(struct in_addr));
                                                case AF_INET6:
                                                        return add_rtattr(m, type, data, sizeof(struct in6_addr));
                                                default:
                                                        return -EINVAL;
                                        }
                                case RTA_TABLE:
                                case RTA_PRIORITY:
                                case RTA_IIF:
                                case RTA_OIF:
                                        return add_rtattr(m, type, data, sizeof(uint32_t));
                                default:
                                        return -ENOTSUP;
                        }
                default:
                        return -ENOTSUP;
        }
}

int sd_rtnl_message_open_container(sd_rtnl_message *m, unsigned short type) {
        uint16_t rtm_type;

        assert_return(m, -EINVAL);
        assert_return(!m->current_container, -EINVAL);

        sd_rtnl_message_get_type(m, &rtm_type);

        if (message_type_is_link(rtm_type)) {
                if (type == IFLA_LINKINFO)
                        return add_rtattr(m, type, NULL, 0);
                else
                        return -ENOTSUP;
        } else
                return -ENOTSUP;

        return 0;
}

int sd_rtnl_message_close_container(sd_rtnl_message *m) {
        assert_return(m, -EINVAL);
        assert_return(m->current_container, -EINVAL);

        m->current_container = NULL;

        return 0;
}

static int message_read(sd_rtnl_message *m, unsigned short *type, void **data) {
        uint16_t rtm_type;
        int r;

        assert(m);
        assert(m->next_rta);
        assert(type);
        assert(data);

        if (!RTA_OK(m->next_rta, m->remaining_size))
                return 0;

        /* make sure we don't try to read a container
         * TODO: add support for entering containers for reading */
        r = sd_rtnl_message_get_type(m, &rtm_type);
        if (r < 0)
                return r;

        switch (rtm_type) {
                case RTM_NEWLINK:
                case RTM_GETLINK:
                case RTM_SETLINK:
                case RTM_DELLINK:
                        if (m->next_rta->rta_type == IFLA_LINKINFO) {
                                return -EINVAL;
                        }
        }

        *data = RTA_DATA(m->next_rta);
        *type = m->next_rta->rta_type;

        m->next_rta = RTA_NEXT(m->next_rta, m->remaining_size);

        return 1;
}

int sd_rtnl_message_read(sd_rtnl_message *m, unsigned short *type, void **data) {
        uint16_t rtm_type;
        int r;

        assert_return(m, -EINVAL);
        assert_return(data, -EINVAL);

        r = sd_rtnl_message_get_type(m, &rtm_type);
        if (r < 0)
                return r;

        switch (rtm_type) {
                case RTM_NEWLINK:
                case RTM_SETLINK:
                case RTM_DELLINK:
                case RTM_GETLINK:
                        if (!m->next_rta) {
                                struct ifinfomsg *ifi = NLMSG_DATA(m->hdr);

                                m->next_rta = IFLA_RTA(ifi);
                                m->remaining_size = IFLA_PAYLOAD(m->hdr);
                        }
                        break;
                case RTM_NEWADDR:
                case RTM_DELADDR:
                case RTM_GETADDR:
                        if (!m->next_rta) {
                                struct ifaddrmsg *ifa = NLMSG_DATA(m->hdr);

                                m->next_rta = IFA_RTA(ifa);
                                m->remaining_size = IFA_PAYLOAD(m->hdr);
                        }
                        break;
                case RTM_NEWROUTE:
                case RTM_DELROUTE:
                case RTM_GETROUTE:
                        if (!m->next_rta) {
                                struct rtmesg *rtm = NLMSG_DATA(m->hdr);

                                m->next_rta = RTM_RTA(rtm);
                                m->remaining_size = RTM_PAYLOAD(m->hdr);
                        }
                        break;
                default:
                        return -ENOTSUP;
        }

        return message_read(m, type, data);
}

uint32_t message_get_serial(sd_rtnl_message *m) {
        assert(m);
        assert(m->hdr);

        return m->hdr->nlmsg_seq;
}

int sd_rtnl_message_get_errno(sd_rtnl_message *m) {
        struct nlmsgerr *err;

        assert_return(m, -EINVAL);
        assert_return(m->hdr, -EINVAL);

        if (m->hdr->nlmsg_type != NLMSG_ERROR)
                return 0;

        err = NLMSG_DATA(m->hdr);

        return err->error;
}

int message_seal(sd_rtnl *nl, sd_rtnl_message *m) {
        assert(nl);
        assert(m);
        assert(m->hdr);

        if (m->sealed)
                return -EPERM;

        m->hdr->nlmsg_seq = nl->serial++;
        m->sealed = true;

        return 0;
}

static int message_receive_need(sd_rtnl *rtnl, size_t *need) {
        assert(rtnl);
        assert(need);

        /* ioctl(rtnl->fd, FIONREAD, &need)
           Does not appear to work on netlink sockets. libnl uses
           MSG_PEEK instead. I don't know if that is worth the
           extra roundtrip.

           For now we simply use the maximum message size the kernel
           may use (NLMSG_GOODSIZE), and then realloc to the actual
           size after reading the message (hence avoiding huge memory
           usage in case many small messages are kept around) */
        *need = page_size();
        if (*need > 8192UL)
                *need = 8192UL;

        return 0;
}

/* returns the number of bytes sent, or a negative error code */
int socket_write_message(sd_rtnl *nl, sd_rtnl_message *m) {
        union {
                struct sockaddr sa;
                struct sockaddr_nl nl;
        } addr = {
                .nl.nl_family = AF_NETLINK,
        };
        ssize_t k;

        assert(nl);
        assert(m);
        assert(m->hdr);

        k = sendto(nl->fd, m->hdr, m->hdr->nlmsg_len,
                        0, &addr.sa, sizeof(addr));
        if (k < 0)
                return (errno == EAGAIN) ? 0 : -errno;

        return k;
}

/* On success, the number of bytes received is returned and *ret points to the received message
 * which has a valid header and the correct size.
 * If nothing useful was received 0 is returned.
 * On failure, a negative error code is returned.
 */
int socket_read_message(sd_rtnl *nl, sd_rtnl_message **ret) {
        sd_rtnl_message *m;
        union {
                struct sockaddr sa;
                struct sockaddr_nl nl;
        } addr;
        socklen_t addr_len;
        int r;
        ssize_t k;
        size_t need;

        assert(nl);
        assert(ret);

        r = message_receive_need(nl, &need);
        if (r < 0)
                return r;

        r = message_new(&m, need);
        if (r < 0)
                return r;

        addr_len = sizeof(addr);

        k = recvfrom(nl->fd, m->hdr, need,
                        0, &addr.sa, &addr_len);
        if (k < 0)
                k = (errno == EAGAIN) ? 0 : -errno; /* no data */
        else if (k == 0)
                k = -ECONNRESET; /* connection was closed by the kernel */
        else if (addr_len != sizeof(addr.nl) ||
                        addr.nl.nl_family != AF_NETLINK)
                k = -EIO; /* not a netlink message */
        else if (addr.nl.nl_pid != 0)
                k = 0; /* not from the kernel */
        else if ((size_t) k < sizeof(struct nlmsghdr) ||
                        (size_t) k < m->hdr->nlmsg_len)
                k = -EIO; /* too small (we do accept too big though) */
        else if (m->hdr->nlmsg_pid && m->hdr->nlmsg_pid != nl->sockaddr.nl.nl_pid)
                k = 0; /* not broadcast and not for us */

        if (k > 0)
                switch (m->hdr->nlmsg_type) {
                        /* check that the size matches the message type */
                        case NLMSG_ERROR:
                                if (m->hdr->nlmsg_len < NLMSG_LENGTH(sizeof(struct nlmsgerr)))
                                        k = -EIO;
                                break;
                        case RTM_NEWLINK:
                        case RTM_SETLINK:
                        case RTM_DELLINK:
                        case RTM_GETLINK:
                                if (m->hdr->nlmsg_len < NLMSG_LENGTH(sizeof(struct ifinfomsg)))
                                        k = -EIO;
                                break;
                        case RTM_NEWADDR:
                        case RTM_DELADDR:
                        case RTM_GETADDR:
                                if (m->hdr->nlmsg_len < NLMSG_LENGTH(sizeof(struct ifaddrmsg)))
                                        k = -EIO;
                                break;
                        case RTM_NEWROUTE:
                        case RTM_DELROUTE:
                        case RTM_GETROUTE:
                                if (m->hdr->nlmsg_len < NLMSG_LENGTH(sizeof(struct rtmsg)))
                                        k = -EIO;
                                break;
                        case NLMSG_NOOP:
                                k = 0;
                                break;
                        default:
                                k = 0; /* ignoring message of unknown type */
                }

        if (k <= 0)
                sd_rtnl_message_unref(m);
        else {
                /* we probably allocated way too much memory, give it back */
                m->hdr = realloc(m->hdr, m->hdr->nlmsg_len);
                *ret = m;
        }

        return k;
}
