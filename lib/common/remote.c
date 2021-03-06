/*
 * Copyright 2008-2020 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 *
 * This source code is licensed under the GNU Lesser General Public License
 * version 2.1 or later (LGPLv2.1+) WITHOUT ANY WARRANTY.
 */

#include <crm_internal.h>
#include <crm/crm.h>

#include <sys/param.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <stdlib.h>
#include <errno.h>
#include <inttypes.h>  /* X32T ~ PRIx32 */

#include <glib.h>
#include <bzlib.h>

#include <crm/common/ipcs_internal.h>
#include <crm/common/xml.h>
#include <crm/common/mainloop.h>
#include <crm/common/remote_internal.h>

#ifdef HAVE_GNUTLS_GNUTLS_H
#  undef KEYFILE
#  include <gnutls/gnutls.h>

const int psk_tls_kx_order[] = {
    GNUTLS_KX_DHE_PSK,
    GNUTLS_KX_PSK,
};

const int anon_tls_kx_order[] = {
    GNUTLS_KX_ANON_DH,
    GNUTLS_KX_DHE_RSA,
    GNUTLS_KX_DHE_DSS,
    GNUTLS_KX_RSA,
    0
};
#endif

/* Swab macros from linux/swab.h */
#ifdef HAVE_LINUX_SWAB_H
#  include <linux/swab.h>
#else
/*
 * casts are necessary for constants, because we never know how for sure
 * how U/UL/ULL map to __u16, __u32, __u64. At least not in a portable way.
 */
#define __swab16(x) ((uint16_t)(                                      \
        (((uint16_t)(x) & (uint16_t)0x00ffU) << 8) |                  \
        (((uint16_t)(x) & (uint16_t)0xff00U) >> 8)))

#define __swab32(x) ((uint32_t)(                                      \
        (((uint32_t)(x) & (uint32_t)0x000000ffUL) << 24) |            \
        (((uint32_t)(x) & (uint32_t)0x0000ff00UL) <<  8) |            \
        (((uint32_t)(x) & (uint32_t)0x00ff0000UL) >>  8) |            \
        (((uint32_t)(x) & (uint32_t)0xff000000UL) >> 24)))

#define __swab64(x) ((uint64_t)(                                      \
        (((uint64_t)(x) & (uint64_t)0x00000000000000ffULL) << 56) |   \
        (((uint64_t)(x) & (uint64_t)0x000000000000ff00ULL) << 40) |   \
        (((uint64_t)(x) & (uint64_t)0x0000000000ff0000ULL) << 24) |   \
        (((uint64_t)(x) & (uint64_t)0x00000000ff000000ULL) <<  8) |   \
        (((uint64_t)(x) & (uint64_t)0x000000ff00000000ULL) >>  8) |   \
        (((uint64_t)(x) & (uint64_t)0x0000ff0000000000ULL) >> 24) |   \
        (((uint64_t)(x) & (uint64_t)0x00ff000000000000ULL) >> 40) |   \
        (((uint64_t)(x) & (uint64_t)0xff00000000000000ULL) >> 56)))
#endif

#define REMOTE_MSG_VERSION 1
#define ENDIAN_LOCAL 0xBADADBBD

struct crm_remote_header_v0 
{
    uint32_t endian;    /* Detect messages from hosts with different endian-ness */
    uint32_t version;
    uint64_t id;
    uint64_t flags;
    uint32_t size_total;
    uint32_t payload_offset;
    uint32_t payload_compressed;
    uint32_t payload_uncompressed;

        /* New fields get added here */

} __attribute__ ((packed));

static struct crm_remote_header_v0 *
crm_remote_header(pcmk__remote_t *remote)
{
    struct crm_remote_header_v0 *header = (struct crm_remote_header_v0 *)remote->buffer;
    if(remote->buffer_offset < sizeof(struct crm_remote_header_v0)) {
        return NULL;

    } else if(header->endian != ENDIAN_LOCAL) {
        uint32_t endian = __swab32(header->endian);

        CRM_LOG_ASSERT(endian == ENDIAN_LOCAL);
        if(endian != ENDIAN_LOCAL) {
            crm_err("Invalid message detected, endian mismatch: %" X32T
                    " is neither %" X32T " nor the swab'd %" X32T,
                    ENDIAN_LOCAL, header->endian, endian);
            return NULL;
        }

        header->id = __swab64(header->id);
        header->flags = __swab64(header->flags);
        header->endian = __swab32(header->endian);

        header->version = __swab32(header->version);
        header->size_total = __swab32(header->size_total);
        header->payload_offset = __swab32(header->payload_offset);
        header->payload_compressed = __swab32(header->payload_compressed);
        header->payload_uncompressed = __swab32(header->payload_uncompressed);
    }

    return header;
}

#ifdef HAVE_GNUTLS_GNUTLS_H

int
crm_initiate_client_tls_handshake(pcmk__remote_t *remote, int timeout_ms)
{
    int rc = 0;
    int pollrc = 0;
    time_t start = time(NULL);

    do {
        rc = gnutls_handshake(*remote->tls_session);
        if (rc == GNUTLS_E_INTERRUPTED || rc == GNUTLS_E_AGAIN) {
            pollrc = crm_remote_ready(remote, 1000);
            if (pollrc < 0) {
                /* poll returned error, there is no hope */
                rc = -1;
            }
        }

    } while (((time(NULL) - start) < (timeout_ms / 1000)) &&
             (rc == GNUTLS_E_INTERRUPTED || rc == GNUTLS_E_AGAIN));

    if (rc < 0) {
        crm_trace("gnutls_handshake() failed with %d", rc);
    }
    return rc;
}

/*!
 * \internal
 * \brief Set minimum prime size required by TLS client
 *
 * \param[in] session  TLS session to affect
 */
static void
pcmk__set_minimum_dh_bits(gnutls_session_t *session)
{
    const char *dh_min_bits_s = getenv("PCMK_dh_min_bits");

    if (dh_min_bits_s) {
        int dh_min_bits = crm_parse_int(dh_min_bits_s, "0");

        /* This function is deprecated since GnuTLS 3.1.7, in favor of letting
         * the priority string imply the DH requirements, but this is the only
         * way to give the user control over compatibility with older servers.
         */
        if (dh_min_bits > 0) {
            crm_info("Requiring server use a Diffie-Hellman prime of at least %d bits",
                     dh_min_bits);
            gnutls_dh_set_prime_bits(*session, dh_min_bits);
        }
    }
}

static unsigned int
pcmk__bound_dh_bits(unsigned int dh_bits)
{
    const char *dh_min_bits_s = getenv("PCMK_dh_min_bits");
    const char *dh_max_bits_s = getenv("PCMK_dh_max_bits");
    int dh_min_bits = 0;
    int dh_max_bits = 0;

    if (dh_min_bits_s) {
        dh_min_bits = crm_parse_int(dh_min_bits_s, "0");
    }
    if (dh_max_bits_s) {
        dh_max_bits = crm_parse_int(dh_max_bits_s, "0");
        if ((dh_min_bits > 0) && (dh_max_bits > 0)
            && (dh_max_bits < dh_min_bits)) {
            crm_warn("Ignoring PCMK_dh_max_bits because it is less than PCMK_dh_min_bits");
            dh_max_bits = 0;
        }
    }
    if ((dh_min_bits > 0) && (dh_bits < dh_min_bits)) {
        return dh_min_bits;
    }
    if ((dh_max_bits > 0) && (dh_bits > dh_max_bits)) {
        return dh_max_bits;
    }
    return dh_bits;
}

/*!
 * \internal
 * \brief Initialize a new TLS session
 *
 * \param[in] csock       Connected socket for TLS session
 * \param[in] conn_type   GNUTLS_SERVER or GNUTLS_CLIENT
 * \param[in] cred_type   GNUTLS_CRD_ANON or GNUTLS_CRD_PSK
 * \param[in] credentials TLS session credentials
 *
 * \return Pointer to newly created session object, or NULL on error
 */
gnutls_session_t *
pcmk__new_tls_session(int csock, unsigned int conn_type,
                      gnutls_credentials_type_t cred_type, void *credentials)
{
    int rc = GNUTLS_E_SUCCESS;
    const char *prio_base = NULL;
    char *prio = NULL;
    gnutls_session_t *session = NULL;

    /* Determine list of acceptable ciphers, etc. Pacemaker always adds the
     * values required for its functionality.
     *
     * For an example of anonymous authentication, see:
     * http://www.manpagez.com/info/gnutls/gnutls-2.10.4/gnutls_81.php#Echo-Server-with-anonymous-authentication
     */

    prio_base = getenv("PCMK_tls_priorities");
    if (prio_base == NULL) {
        prio_base = PCMK_GNUTLS_PRIORITIES;
    }
    prio = crm_strdup_printf("%s:%s", prio_base,
                             (cred_type == GNUTLS_CRD_ANON)? "+ANON-DH" : "+DHE-PSK:+PSK");

    session = gnutls_malloc(sizeof(gnutls_session_t));
    if (session == NULL) {
        rc = GNUTLS_E_MEMORY_ERROR;
        goto error;
    }

    rc = gnutls_init(session, conn_type);
    if (rc != GNUTLS_E_SUCCESS) {
        goto error;
    }

    /* @TODO On the server side, it would be more efficient to cache the
     * priority with gnutls_priority_init2() and set it with
     * gnutls_priority_set() for all sessions.
     */
    rc = gnutls_priority_set_direct(*session, prio, NULL);
    if (rc != GNUTLS_E_SUCCESS) {
        goto error;
    }
    if (conn_type == GNUTLS_CLIENT) {
        pcmk__set_minimum_dh_bits(session);
    }

    gnutls_transport_set_ptr(*session,
                             (gnutls_transport_ptr_t) GINT_TO_POINTER(csock));

    rc = gnutls_credentials_set(*session, cred_type, credentials);
    if (rc != GNUTLS_E_SUCCESS) {
        goto error;
    }
    free(prio);
    return session;

error:
    crm_err("Could not initialize %s TLS %s session: %s "
            CRM_XS " rc=%d priority='%s'",
            (cred_type == GNUTLS_CRD_ANON)? "anonymous" : "PSK",
            (conn_type == GNUTLS_SERVER)? "server" : "client",
            gnutls_strerror(rc), rc, prio);
    free(prio);
    if (session != NULL) {
        gnutls_free(session);
    }
    return NULL;
}

/*!
 * \internal
 * \brief Initialize Diffie-Hellman parameters for a TLS server
 *
 * \param[out] dh_params  Parameter object to initialize
 *
 * \return GNUTLS_E_SUCCESS on success, GnuTLS error code on error
 * \todo The current best practice is to allow the client and server to
 *       negotiate the Diffie-Hellman parameters via a TLS extension (RFC 7919).
 *       However, we have to support both older versions of GnuTLS (<3.6) that
 *       don't support the extension on our side, and older Pacemaker versions
 *       that don't support the extension on the other side. The next best
 *       practice would be to use a known good prime (see RFC 5114 section 2.2),
 *       possibly stored in a file distributed with Pacemaker.
 */
int
pcmk__init_tls_dh(gnutls_dh_params_t *dh_params)
{
    int rc = GNUTLS_E_SUCCESS;
    unsigned int dh_bits = 0;

    rc = gnutls_dh_params_init(dh_params);
    if (rc != GNUTLS_E_SUCCESS) {
        goto error;
    }

#ifdef HAVE_GNUTLS_SEC_PARAM_TO_PK_BITS
    dh_bits = gnutls_sec_param_to_pk_bits(GNUTLS_PK_DH,
                                          GNUTLS_SEC_PARAM_NORMAL);
    if (dh_bits == 0) {
        rc = GNUTLS_E_DH_PRIME_UNACCEPTABLE;
        goto error;
    }
#else
    dh_bits = 1024;
#endif
    dh_bits = pcmk__bound_dh_bits(dh_bits);

    crm_info("Generating Diffie-Hellman parameters with %u-bit prime for TLS",
             dh_bits);
    rc = gnutls_dh_params_generate2(*dh_params, dh_bits);
    if (rc != GNUTLS_E_SUCCESS) {
        goto error;
    }

    return rc;

error:
    crm_err("Could not initialize Diffie-Hellman parameters for TLS: %s "
            CRM_XS " rc=%d", gnutls_strerror(rc), rc);
    CRM_ASSERT(rc == GNUTLS_E_SUCCESS);
    return rc;
}

/*!
 * \internal
 * \brief Process handshake data from TLS client
 *
 * Read as much TLS handshake data as is available.
 *
 * \param[in] client  Client connection
 *
 * \retval GnuTLS error code on error
 * \retval 0 if more data is needed
 * \retval 1 if handshake is successfully completed
 */
int
pcmk__read_handshake_data(pcmk__client_t *client)
{
    int rc = 0;

    CRM_ASSERT(client && client->remote && client->remote->tls_session);

    do {
        rc = gnutls_handshake(*client->remote->tls_session);
    } while (rc == GNUTLS_E_INTERRUPTED);

    if (rc == GNUTLS_E_AGAIN) {
        /* No more data is available at the moment. This function should be
         * invoked again once the client sends more.
         */
        return 0;
    } else if (rc != GNUTLS_E_SUCCESS) {
        return rc;
    }
    return 1;
}

static int
crm_send_tls(gnutls_session_t * session, const char *buf, size_t len)
{
    const char *unsent = buf;
    int rc = 0;
    int total_send;

    if (buf == NULL) {
        return -EINVAL;
    }

    total_send = len;
    crm_trace("Message size: %llu", (unsigned long long) len);

    while (TRUE) {
        rc = gnutls_record_send(*session, unsent, len);

        if (rc == GNUTLS_E_INTERRUPTED || rc == GNUTLS_E_AGAIN) {
            crm_trace("Retrying to send %llu bytes",
                      (unsigned long long) len);

        } else if (rc < 0) {
            // Caller can log as error if necessary
            crm_info("TLS connection terminated: %s " CRM_XS " rc=%d",
                     gnutls_strerror(rc), rc);
            rc = -ECONNABORTED;
            break;

        } else if (rc < len) {
            crm_debug("Sent %d of %llu bytes", rc, (unsigned long long) len);
            len -= rc;
            unsent += rc;
        } else {
            crm_trace("Sent all %d bytes", rc);
            break;
        }
    }

    return rc < 0 ? rc : total_send;
}
#endif

static int
crm_send_plaintext(int sock, const char *buf, size_t len)
{

    int rc = 0;
    const char *unsent = buf;
    int total_send;

    if (buf == NULL) {
        return -EINVAL;
    }
    total_send = len;

    crm_trace("Message on socket %d: size=%llu",
              sock, (unsigned long long) len);
  retry:
    rc = write(sock, unsent, len);
    if (rc < 0) {
        rc = -errno;
        switch (errno) {
            case EINTR:
            case EAGAIN:
                crm_trace("Retry");
                goto retry;
            default:
                crm_perror(LOG_INFO,
                           "Could only write %d of the remaining %llu bytes",
                           rc, (unsigned long long) len);
                break;
        }

    } else if (rc < len) {
        crm_trace("Only sent %d of %llu remaining bytes",
                  rc, (unsigned long long) len);
        len -= rc;
        unsent += rc;
        goto retry;

    } else {
        crm_trace("Sent %d bytes: %.100s", rc, buf);
    }

    return rc < 0 ? rc : total_send;

}

static int
crm_remote_sendv(pcmk__remote_t *remote, struct iovec * iov, int iovs)
{
    int rc = 0;

    for (int lpc = 0; (lpc < iovs) && (rc >= 0); lpc++) {
#ifdef HAVE_GNUTLS_GNUTLS_H
        if (remote->tls_session) {
            rc = crm_send_tls(remote->tls_session, iov[lpc].iov_base, iov[lpc].iov_len);
            continue;
        }
#endif
        if (remote->tcp_socket) {
            rc = crm_send_plaintext(remote->tcp_socket, iov[lpc].iov_base, iov[lpc].iov_len);
        } else {
            rc = -ESOCKTNOSUPPORT;
        }
    }
    return rc;
}

int
crm_remote_send(pcmk__remote_t *remote, xmlNode *msg)
{
    int rc = pcmk_ok;
    static uint64_t id = 0;
    char *xml_text = dump_xml_unformatted(msg);

    struct iovec iov[2];
    struct crm_remote_header_v0 *header;

    if (xml_text == NULL) {
        crm_err("Could not send remote message: no message provided");
        return -EINVAL;
    }

    header = calloc(1, sizeof(struct crm_remote_header_v0));
    iov[0].iov_base = header;
    iov[0].iov_len = sizeof(struct crm_remote_header_v0);

    iov[1].iov_base = xml_text;
    iov[1].iov_len = 1 + strlen(xml_text);

    id++;
    header->id = id;
    header->endian = ENDIAN_LOCAL;
    header->version = REMOTE_MSG_VERSION;
    header->payload_offset = iov[0].iov_len;
    header->payload_uncompressed = iov[1].iov_len;
    header->size_total = iov[0].iov_len + iov[1].iov_len;

    crm_trace("Sending len[0]=%d, start=%x",
              (int)iov[0].iov_len, *(int*)(void*)xml_text);
    rc = crm_remote_sendv(remote, iov, 2);
    if (rc < 0) {
        crm_err("Could not send remote message: %s " CRM_XS " rc=%d",
                pcmk_strerror(rc), rc);
    }

    free(iov[0].iov_base);
    free(iov[1].iov_base);
    return rc;
}


/*!
 * \internal
 * \brief handles the recv buffer and parsing out msgs.
 * \note new_data is owned by this function once it is passed in.
 */
xmlNode *
crm_remote_parse_buffer(pcmk__remote_t *remote)
{
    xmlNode *xml = NULL;
    struct crm_remote_header_v0 *header = crm_remote_header(remote);

    if (remote->buffer == NULL || header == NULL) {
        return NULL;
    }

    /* Support compression on the receiving end now, in case we ever want to add it later */
    if (header->payload_compressed) {
        int rc = 0;
        unsigned int size_u = 1 + header->payload_uncompressed;
        char *uncompressed = calloc(1, header->payload_offset + size_u);

        crm_trace("Decompressing message data %d bytes into %d bytes",
                 header->payload_compressed, size_u);

        rc = BZ2_bzBuffToBuffDecompress(uncompressed + header->payload_offset, &size_u,
                                        remote->buffer + header->payload_offset,
                                        header->payload_compressed, 1, 0);

        if (rc != BZ_OK && header->version > REMOTE_MSG_VERSION) {
            crm_warn("Couldn't decompress v%d message, we only understand v%d",
                     header->version, REMOTE_MSG_VERSION);
            free(uncompressed);
            return NULL;

        } else if (rc != BZ_OK) {
            crm_err("Decompression failed: %s " CRM_XS " bzerror=%d",
                    bz2_strerror(rc), rc);
            free(uncompressed);
            return NULL;
        }

        CRM_ASSERT(size_u == header->payload_uncompressed);

        memcpy(uncompressed, remote->buffer, header->payload_offset);       /* Preserve the header */
        remote->buffer_size = header->payload_offset + size_u;

        free(remote->buffer);
        remote->buffer = uncompressed;
        header = crm_remote_header(remote);
    }

    /* take ownership of the buffer */
    remote->buffer_offset = 0;

    CRM_LOG_ASSERT(remote->buffer[sizeof(struct crm_remote_header_v0) + header->payload_uncompressed - 1] == 0);

    xml = string2xml(remote->buffer + header->payload_offset);
    if (xml == NULL && header->version > REMOTE_MSG_VERSION) {
        crm_warn("Couldn't parse v%d message, we only understand v%d",
                 header->version, REMOTE_MSG_VERSION);

    } else if (xml == NULL) {
        crm_err("Couldn't parse: '%.120s'", remote->buffer + header->payload_offset);
    }

    return xml;
}

/*!
 * \internal
 * \brief Wait for a remote session to have data to read
 *
 * \param[in] remote         Connection to check
 * \param[in] total_timeout  Maximum time (in ms) to wait
 *
 * \return Positive value if ready to be read, 0 on timeout, -errno on error
 */
int
crm_remote_ready(pcmk__remote_t *remote, int total_timeout)
{
    struct pollfd fds = { 0, };
    int sock = 0;
    int rc = 0;
    time_t start;
    int timeout = total_timeout;

#ifdef HAVE_GNUTLS_GNUTLS_H
    if (remote->tls_session) {
        void *sock_ptr = gnutls_transport_get_ptr(*remote->tls_session);

        sock = GPOINTER_TO_INT(sock_ptr);
    } else if (remote->tcp_socket) {
#else
    if (remote->tcp_socket) {
#endif
        sock = remote->tcp_socket;
    } else {
        crm_err("Unsupported connection type");
    }

    if (sock <= 0) {
        crm_trace("No longer connected");
        return -ENOTCONN;
    }

    start = time(NULL);
    errno = 0;
    do {
        fds.fd = sock;
        fds.events = POLLIN;

        /* If we got an EINTR while polling, and we have a
         * specific timeout we are trying to honor, attempt
         * to adjust the timeout to the closest second. */
        if (errno == EINTR && (timeout > 0)) {
            timeout = total_timeout - ((time(NULL) - start) * 1000);
            if (timeout < 1000) {
                timeout = 1000;
            }
        }

        rc = poll(&fds, 1, timeout);
    } while (rc < 0 && errno == EINTR);

    return (rc < 0)? -errno : rc;
}


/*!
 * \internal
 * \brief Read bytes off non blocking remote connection.
 *
 * \note only use with NON-Blocking sockets. Should only be used after polling socket.
 *       This function will return once max_size is met, the socket read buffer
 *       is empty, or an error is encountered.
 *
 * \retval number of bytes received
 */
static size_t
crm_remote_recv_once(pcmk__remote_t *remote)
{
    int rc = 0;
    size_t read_len = sizeof(struct crm_remote_header_v0);
    struct crm_remote_header_v0 *header = crm_remote_header(remote);

    if(header) {
        /* Stop at the end of the current message */
        read_len = header->size_total;
    }

    /* automatically grow the buffer when needed */
    if(remote->buffer_size < read_len) {
           remote->buffer_size = 2 * read_len;
        crm_trace("Expanding buffer to %llu bytes",
                  (unsigned long long) remote->buffer_size);

        remote->buffer = realloc_safe(remote->buffer, remote->buffer_size + 1);
        CRM_ASSERT(remote->buffer != NULL);
    }

#ifdef HAVE_GNUTLS_GNUTLS_H
    if (remote->tls_session) {
        rc = gnutls_record_recv(*(remote->tls_session),
                                remote->buffer + remote->buffer_offset,
                                remote->buffer_size - remote->buffer_offset);
        if (rc == GNUTLS_E_INTERRUPTED) {
            rc = -EINTR;
        } else if (rc == GNUTLS_E_AGAIN) {
            rc = -EAGAIN;
        } else if (rc < 0) {
            crm_debug("TLS receive failed: %s (%d)", gnutls_strerror(rc), rc);
            rc = -pcmk_err_generic;
        }
    } else if (remote->tcp_socket) {
#else
    if (remote->tcp_socket) {
#endif
        errno = 0;
        rc = read(remote->tcp_socket,
                  remote->buffer + remote->buffer_offset,
                  remote->buffer_size - remote->buffer_offset);
        if(rc < 0) {
            rc = -errno;
        }

    } else {
        crm_err("Unsupported connection type");
        return -ESOCKTNOSUPPORT;
    }

    /* process any errors. */
    if (rc > 0) {
        remote->buffer_offset += rc;
        /* always null terminate buffer, the +1 to alloc always allows for this. */
        remote->buffer[remote->buffer_offset] = '\0';
        crm_trace("Received %u more bytes, %llu total",
                  rc, (unsigned long long) remote->buffer_offset);

    } else if (rc == -EINTR || rc == -EAGAIN) {
        crm_trace("non-blocking, exiting read: %s (%d)", pcmk_strerror(rc), rc);

    } else if (rc == 0) {
        crm_debug("EOF encoutered after %llu bytes",
                  (unsigned long long) remote->buffer_offset);
        return -ENOTCONN;

    } else {
        crm_debug("Error receiving message after %llu bytes: %s (%d)",
                  (unsigned long long) remote->buffer_offset,
                  pcmk_strerror(rc), rc);
        return -ENOTCONN;
    }

    header = crm_remote_header(remote);
    if(header) {
        if(remote->buffer_offset < header->size_total) {
            crm_trace("Read less than the advertised length: %llu < %u bytes",
                      (unsigned long long) remote->buffer_offset,
                      header->size_total);
        } else {
            crm_trace("Read full message of %llu bytes",
                      (unsigned long long) remote->buffer_offset);
            return remote->buffer_offset;
        }
    }

    return -EAGAIN;
}

/*!
 * \internal
 * \brief Read message(s) from a remote connection
 *
 * \param[in]  remote         Remote connection to read
 * \param[in]  total_timeout  Fail if message not read in this time (ms)
 * \param[out] disconnected   Will be set to 1 if disconnect detected
 *
 * \return TRUE if at least one full message read, FALSE otherwise
 */
gboolean
crm_remote_recv(pcmk__remote_t *remote, int total_timeout, int *disconnected)
{
    int rc;
    time_t start = time(NULL);
    int remaining_timeout = 0;

    if (total_timeout == 0) {
        total_timeout = 10000;
    } else if (total_timeout < 0) {
        total_timeout = 60000;
    }
    *disconnected = 0;

    remaining_timeout = total_timeout;
    while ((remaining_timeout > 0) && !(*disconnected)) {

        crm_trace("Waiting for remote data (%d of %d ms timeout remaining)",
                  remaining_timeout, total_timeout);
        rc = crm_remote_ready(remote, remaining_timeout);

        if (rc == 0) {
            crm_err("Timed out (%d ms) while waiting for remote data",
                    remaining_timeout);
            return FALSE;

        } else if (rc < 0) {
            crm_debug("Wait for remote data aborted, will try again: %s "
                      CRM_XS " rc=%d", pcmk_strerror(rc), rc);

        } else {
            rc = crm_remote_recv_once(remote);
            if (rc > 0) {
                return TRUE;
            } else if (rc == -EAGAIN) {
                crm_trace("Still waiting for remote data");
            } else if (rc < 0) {
                crm_debug("Could not receive remote data: %s " CRM_XS " rc=%d",
                          pcmk_strerror(rc), rc);
            }
        }

        if (rc == -ENOTCONN) {
            *disconnected = 1;
            return FALSE;
        }

        remaining_timeout = total_timeout - ((time(NULL) - start) * 1000);
    }

    return FALSE;
}

struct tcp_async_cb_data {
    gboolean success;
    int sock;
    void *userdata;
    void (*callback) (void *userdata, int sock);
    int timeout;                /*ms */
    time_t start;
};

static gboolean
check_connect_finished(gpointer userdata)
{
    struct tcp_async_cb_data *cb_data = userdata;
    int cb_arg = 0; // socket fd on success, -errno on error
    int sock = cb_data->sock;
    int error = 0;

    fd_set rset, wset;
    socklen_t len = sizeof(error);
    struct timeval ts = { 0, };

    if (cb_data->success == TRUE) {
        goto dispatch_done;
    }

    FD_ZERO(&rset);
    FD_SET(sock, &rset);
    wset = rset;

    crm_trace("fd %d: checking to see if connect finished", sock);
    cb_arg = select(sock + 1, &rset, &wset, NULL, &ts);

    if (cb_arg < 0) {
        cb_arg = -errno;
        if ((errno == EINPROGRESS) || (errno == EAGAIN)) {
            /* reschedule if there is still time left */
            if ((time(NULL) - cb_data->start) < (cb_data->timeout / 1000)) {
                goto reschedule;
            } else {
                cb_arg = -ETIMEDOUT;
            }
        }
        crm_trace("fd %d: select failed %d connect dispatch ", sock, cb_arg);
        goto dispatch_done;
    } else if (cb_arg == 0) {
        if ((time(NULL) - cb_data->start) < (cb_data->timeout / 1000)) {
            goto reschedule;
        }
        crm_debug("fd %d: timeout during select", sock);
        cb_arg = -ETIMEDOUT;
        goto dispatch_done;
    } else {
        crm_trace("fd %d: select returned success", sock);
        cb_arg = 0;
    }

    /* can we read or write to the socket now? */
    if (FD_ISSET(sock, &rset) || FD_ISSET(sock, &wset)) {
        if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &len) < 0) {
            cb_arg = -errno;
            crm_trace("fd %d: call to getsockopt failed", sock);
            goto dispatch_done;
        }
        if (error) {
            crm_trace("fd %d: error returned from getsockopt: %d", sock, error);
            cb_arg = -error;
            goto dispatch_done;
        }
    } else {
        crm_trace("neither read nor write set after select");
        cb_arg = -EAGAIN;
        goto dispatch_done;
    }

  dispatch_done:
    if (!cb_arg) {
        crm_trace("fd %d: connected", sock);
        /* Success, set the return code to the sock to report to the callback */
        cb_arg = cb_data->sock;
        cb_data->sock = 0;
    } else {
        close(sock);
    }

    if (cb_data->callback) {
        cb_data->callback(cb_data->userdata, cb_arg);
    }
    free(cb_data);
    return FALSE;

  reschedule:

    /* will check again next interval */
    return TRUE;
}

static int
internal_tcp_connect_async(int sock,
                           const struct sockaddr *addr, socklen_t addrlen, int timeout /* ms */ ,
                           int *timer_id, void *userdata, void (*callback) (void *userdata, int sock))
{
    int rc = 0;
    int interval = 500;
    int timer;
    struct tcp_async_cb_data *cb_data = NULL;

    rc = pcmk__set_nonblocking(sock);
    if (rc != pcmk_rc_ok) {
        crm_warn("Could not set socket non-blocking: %s " CRM_XS " rc=%d",
                 pcmk_rc_str(rc), rc);
        close(sock);
        return -1;
    }

    rc = connect(sock, addr, addrlen);
    if (rc < 0 && (errno != EINPROGRESS) && (errno != EAGAIN)) {
        crm_perror(LOG_WARNING, "connect");
        return -1;
    }

    cb_data = calloc(1, sizeof(struct tcp_async_cb_data));
    cb_data->userdata = userdata;
    cb_data->callback = callback;
    cb_data->sock = sock;
    cb_data->timeout = timeout;
    cb_data->start = time(NULL);

    if (rc == 0) {
        /* The connect was successful immediately, we still return to mainloop
         * and let this callback get called later. This avoids the user of this api
         * to have to account for the fact the callback could be invoked within this
         * function before returning. */
        cb_data->success = TRUE;
        interval = 1;
    }

    /* Check connect finished is mostly doing a non-block poll on the socket
     * to see if we can read/write to it. Once we can, the connect has completed.
     * This method allows us to connect to the server without blocking mainloop.
     *
     * This is a poor man's way of polling to see when the connection finished.
     * At some point we should figure out a way to use a mainloop fd callback for this.
     * Something about the way mainloop is currently polling prevents this from working at the
     * moment though. */
    crm_trace("Scheduling check in %dms for whether connect to fd %d finished",
              interval, sock);
    timer = g_timeout_add(interval, check_connect_finished, cb_data);
    if (timer_id) {
        *timer_id = timer;
    }

    return 0;
}

static int
internal_tcp_connect(int sock, const struct sockaddr *addr, socklen_t addrlen)
{
    int rc = connect(sock, addr, addrlen);

    if (rc < 0) {
        rc = -errno;
        crm_warn("Could not connect socket: %s " CRM_XS " rc=%d",
                 pcmk_strerror(rc), rc);
        return rc;
    }

    rc = pcmk__set_nonblocking(sock);
    if (rc != pcmk_rc_ok) {
        crm_warn("Could not set socket non-blocking: %s " CRM_XS " rc=%d",
                 pcmk_rc_str(rc), rc);
        return pcmk_rc2legacy(rc);
    }

    return pcmk_ok;
}

/*!
 * \internal
 * \brief Connect to server at specified TCP port
 *
 * \param[in]  host      Name of server to connect to
 * \param[in]  port      Server port to connect to
 * \param[in]  timeout   Report error if not connected in this many milliseconds
 * \param[out] timer_id  If non-NULL, will be set to timer ID, if asynchronous
 * \param[in]  userdata  Data to pass to callback, if asynchronous
 * \param[in]  callback  If non-NULL, connect asynchronously then call this
 *
 * \return File descriptor of connected socket on success, -ENOTCONN otherwise
 */
int
crm_remote_tcp_connect_async(const char *host, int port, int timeout,
                             int *timer_id, void *userdata,
                             void (*callback) (void *userdata, int sock))
{
    char buffer[INET6_ADDRSTRLEN];
    struct addrinfo *res = NULL;
    struct addrinfo *rp = NULL;
    struct addrinfo hints;
    const char *server = host;
    int ret_ga;
    int sock = -ENOTCONN;

    // Get host's IP address(es)
    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_family = AF_UNSPEC;        /* Allow IPv4 or IPv6 */
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_CANONNAME;
    ret_ga = getaddrinfo(server, NULL, &hints, &res);
    if (ret_ga) {
        crm_err("Unable to get IP address info for %s: %s",
                server, gai_strerror(ret_ga));
        goto async_cleanup;
    }
    if (!res || !res->ai_addr) {
        crm_err("Unable to get IP address info for %s: no result", server);
        goto async_cleanup;
    }

    // getaddrinfo() returns a list of host's addresses, try them in order
    for (rp = res; rp != NULL; rp = rp->ai_next) {
        struct sockaddr *addr = rp->ai_addr;

        if (!addr) {
            continue;
        }

        if (rp->ai_canonname) {
            server = res->ai_canonname;
        }
        crm_debug("Got canonical name %s for %s", server, host);

        sock = socket(rp->ai_family, SOCK_STREAM, IPPROTO_TCP);
        if (sock == -1) {
            crm_perror(LOG_WARNING, "creating socket for connection to %s",
                       server);
            sock = -ENOTCONN;
            continue;
        }

        /* Set port appropriately for address family */
        /* (void*) casts avoid false-positive compiler alignment warnings */
        if (addr->sa_family == AF_INET6) {
            ((struct sockaddr_in6 *)(void*)addr)->sin6_port = htons(port);
        } else {
            ((struct sockaddr_in *)(void*)addr)->sin_port = htons(port);
        }

        memset(buffer, 0, DIMOF(buffer));
        crm_sockaddr2str(addr, buffer);
        crm_info("Attempting TCP connection to %s:%d", buffer, port);

        if (callback) {
            if (internal_tcp_connect_async
                (sock, rp->ai_addr, rp->ai_addrlen, timeout, timer_id, userdata, callback) == 0) {
                goto async_cleanup; /* Success for now, we'll hear back later in the callback */
            }

        } else if (internal_tcp_connect(sock, rp->ai_addr, rp->ai_addrlen) == 0) {
            break;          /* Success */
        }

        close(sock);
        sock = -ENOTCONN;
    }

async_cleanup:

    if (res) {
        freeaddrinfo(res);
    }
    return sock;
}

int
crm_remote_tcp_connect(const char *host, int port)
{
    return crm_remote_tcp_connect_async(host, port, -1, NULL, NULL, NULL);
}

/*!
 * \brief Convert an IP address (IPv4 or IPv6) to a string for logging
 *
 * \param[in]  sa  Socket address for IP
 * \param[out] s   Storage for at least INET6_ADDRSTRLEN bytes
 *
 * \note sa The socket address can be a pointer to struct sockaddr_in (IPv4),
 *          struct sockaddr_in6 (IPv6) or struct sockaddr_storage (either),
 *          as long as its sa_family member is set correctly.
 */
void
crm_sockaddr2str(void *sa, char *s)
{
    switch (((struct sockaddr*)sa)->sa_family) {
        case AF_INET:
            inet_ntop(AF_INET, &(((struct sockaddr_in *)sa)->sin_addr),
                      s, INET6_ADDRSTRLEN);
            break;

        case AF_INET6:
            inet_ntop(AF_INET6, &(((struct sockaddr_in6 *)sa)->sin6_addr),
                      s, INET6_ADDRSTRLEN);
            break;

        default:
            strcpy(s, "<invalid>");
    }
}

int
crm_remote_accept(int ssock)
{
    int csock = 0;
    int rc = 0;
    unsigned laddr = 0;
    struct sockaddr_storage addr;
    char addr_str[INET6_ADDRSTRLEN];
#ifdef TCP_USER_TIMEOUT
    int optval;
    long sbd_timeout = crm_get_sbd_timeout();
#endif

    /* accept the connection */
    laddr = sizeof(addr);
    memset(&addr, 0, sizeof(addr));
    csock = accept(ssock, (struct sockaddr *)&addr, &laddr);
    crm_sockaddr2str(&addr, addr_str);
    crm_info("New remote connection from %s", addr_str);

    if (csock == -1) {
        crm_err("accept socket failed");
        return -1;
    }

    rc = pcmk__set_nonblocking(csock);
    if (rc != pcmk_rc_ok) {
        crm_err("Could not set socket non-blocking: %s " CRM_XS " rc=%d",
                pcmk_rc_str(rc), rc);
        close(csock);
        return pcmk_rc2legacy(rc);
    }

#ifdef TCP_USER_TIMEOUT
    if (sbd_timeout > 0) {
        optval = sbd_timeout / 2; /* time to fail and retry before watchdog */
        rc = setsockopt(csock, SOL_TCP, TCP_USER_TIMEOUT,
                        &optval, sizeof(optval));
        if (rc < 0) {
            crm_err("setting TCP_USER_TIMEOUT (%d) on client socket failed",
                    optval);
            close(csock);
            return rc;
        }
    }
#endif

    return csock;
}

/*!
 * \brief Get the default remote connection TCP port on this host
 *
 * \return Remote connection TCP port number
 */
int
crm_default_remote_port()
{
    static int port = 0;

    if (port == 0) {
        const char *env = getenv("PCMK_remote_port");

        if (env) {
            errno = 0;
            port = strtol(env, NULL, 10);
            if (errno || (port < 1) || (port > 65535)) {
                crm_warn("Environment variable PCMK_remote_port has invalid value '%s', using %d instead",
                         env, DEFAULT_REMOTE_PORT);
                port = DEFAULT_REMOTE_PORT;
            }
        } else {
            port = DEFAULT_REMOTE_PORT;
        }
    }
    return port;
}
