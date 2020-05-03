#include "snowhttp.h"

#include <iostream>

#include <cassert>
#include <cerrno>
#include <climits>
#include <netinet/tcp.h>

bool buff_put(struct buff_static_t *buff, char *data, size_t dataSize) {
    if (buff->head + dataSize > BUFFSIZE)
        return false;

    memcpy(&buff->buff[buff->head], data, dataSize);
    buff->head += dataSize;
    return true;
}

bool buff_pull(struct buff_static_t *buff, void *dest, size_t size) {
    if (buff->tail + size > BUFFSIZE)
        return false;

    buff->tail += size;
    memcpy(dest, &buff->buff[buff->tail], size);
    return true;
}

int buff_to_pull(struct buff_static_t *buff) {
    return buff->head - buff->tail;
}

bool buff_empty(struct buff_static_t *buff) {
    return buff->head == buff->tail;
}

size_t buff_pull_to_sock(struct buff_static_t *buff, void *f, size_t size, bool ssl) {
    size_t remain;

    if (buff->tail + size > BUFFSIZE) {
        std::cerr << "ERR: send buffer too small\n";
        assert(0);
    }

    remain = size;

    while (remain > 0) {
        ssize_t ret;

        if (ssl) {
            ret = wolfSSL_write((WOLFSSL *) f, &buff->buff[buff->tail], remain);

            if (ret == -1) {
                int err = wolfSSL_get_error((WOLFSSL *) f, ret);

                if (err == WOLFSSL_ERROR_WANT_WRITE)
                    break;
                else {
                    char buffer[256];
                    fprintf(stderr, "sock put error = %d, %s\n", err, wolfSSL_ERR_error_string(err, buffer));
                    assert(0);
                }
            }
        } else {
            ret = write(*(int *) f, &buff->buff[buff->tail], remain);
            if (ret < 0) {
                if (errno == EINTR)
                    continue;

                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOTCONN)
                    break;

                return -1;
            }
        }

        remain -= ret;
        buff->tail += ret;
    }

    return remain;
}

size_t buff_put_from_sock(struct buff_static_t *buff, void *f, int size, bool ssl) {

    size_t remain, total = 0;

    if (size < 0)
        size = INT_MAX;

    remain = size;

    while (remain) {
        ssize_t ret;
        size_t head_room = BUFFSIZE - buff->head;

        if (head_room <= 0) {
            std::cerr << "ERR: receive buffer too small\n";
            assert(0);
        }

        if (ssl) {
            ret = wolfSSL_read((WOLFSSL *) f, &buff->buff[buff->head], head_room);

            if (ret == -1) {
                int err = wolfSSL_get_error((WOLFSSL *) f, ret);

                if (err == WOLFSSL_ERROR_WANT_READ)
                    break;
                else {
                    char buffer[256];
                    fprintf(stderr, "sock put error = %d, %s\n", err, wolfSSL_ERR_error_string(err, buffer));
                    assert(0);
                }
            }
        } else {
            ret = read(*(int *) f, &buff->buff[buff->head], head_room);
            if (ret < 0) {
                if (errno == EINTR)
                    continue;

                if (errno == EAGAIN || errno == ENOTCONN || errno == EWOULDBLOCK)
                    break;

                assert(0);
            }

        }

        if (!ret) {
            assert(0); // conn closed?
            break;
        }

        assert(ret > 0);

        buff->head += ret;
        remain -= ret;
        total += ret;

    }
    return total;
}

void snow_parseUrl(snow_connection_t *conn) {
    char *protocol_end = strstr(conn->requestUrl, "://");
    if (protocol_end != nullptr) {
        conn->protocol = conn->requestUrl;
        *protocol_end = 0;
    } else
        assert(0);

    char *it;
    for (it = protocol_end + 3; *it != ':' && *it != '/' && it != nullptr; it++);

    if (it == nullptr) assert(0);

    if (*it == ':') { // we have port
        conn->hostname = protocol_end + 3;
        *it = 0;
        conn->path = strchr(it + 1, '/');

        if (conn->path == nullptr) assert(0);
        *conn->path = 0;  // set slash to 0
        conn->path++;

        conn->port = atoi(it + 1);

    } else { // no port
        conn->hostname = protocol_end + 3;
        *it = 0; // set slash to 0
        conn->path = it + 1;
    }

    if (conn->port == 0) {
        if (strcmp(conn->protocol, "http") == 0)
            conn->port = 80, conn->secure = false;
        else if (strcmp(conn->protocol, "https") == 0)
            conn->port = 443, conn->secure = true;
        else
            assert(0);
    }
}

void snow_resolveHost(snow_connection_t *conn) {
    int error;
    struct hostent *result;

    gethostbyname_r(conn->hostname, &conn->hostent, conn->hostentBuff, 2048, &result, &error);

    if (SNOW_UNLIKELY(result == nullptr)) {
        std::cerr << "Hostname resolve error: " << error << "\n";
        assert(0);
    }

    bzero(&conn->address, sizeof(conn->address));

    memcpy(&conn->address.sin_addr, conn->hostent.h_addr_list[0], conn->hostent.h_length);
    conn->address.sin_family = AF_INET;
    conn->address.sin_port = htons(conn->port);
}

void snow_startTLSHandshake(snow_global_t *global, snow_connection_t *conn) {
    if ((SNOW_UNLIKELY((conn->ssl = wolfSSL_new(global->wolfCtx)) == nullptr))) {
        fprintf(stderr, "wolfSSL_new error.\n");
        assert(0);
    }

    wolfSSL_set_fd(conn->ssl, conn->sockfd);
    wolfSSL_set_using_nonblock(conn->ssl, 1);

    conn->connectionStatus = CONN_TLS_HANDSHAKE; // will be processed in write cb & read cb
}

static void snow_io_read_cb(struct ev_loop *loop, struct ev_io *w, int revents) {
    auto *conn = (struct snow_connection_t *) ((struct ev_io_snow *) w)->data;

    if (SNOW_LIKELY(conn->connectionStatus == CONN_WAITING || conn->connectionStatus == CONN_RECEIVING)) {
        size_t readSize = buff_put_from_sock(&conn->readBuff, conn->secure ? (void *) conn->ssl : (void *) &conn->sockfd, -1, conn->secure);

        if (!readSize) return; // no read

        if (conn->connectionStatus == CONN_WAITING) {
            conn->connectionStatus = CONN_RECEIVING;

            char *chunked = strstr(&conn->readBuff.buff[conn->readBuff.tail], "Transfer-Encoding: chunked\r\n");

            if (chunked) conn->chunked = true;

            char *p = strstr(&conn->readBuff.buff[conn->readBuff.tail], "\r\n\r\n");

            if (SNOW_UNLIKELY(p == nullptr)) {
                std::cerr << "ERR: could not find the end of headers\n";
                assert(0);
            }

            p += 4; // skip over \r\n\r\n

            conn->readBuff.tail = p - conn->readBuff.buff;
            conn->content = p;

        }

        if ((conn->chunked)) {
            if (strcmp(&conn->readBuff.buff[conn->readBuff.head - 5], "0\r\n\r\n") == 0) {
                char *chunkBegin = conn->content;
                char *newCopyStart = chunkBegin;

                // parse chunks & verify sizes
                while (chunkBegin < conn->readBuff.buff + conn->readBuff.head) {
                    size_t chunkLen = strtol(chunkBegin, nullptr, 16);
                    conn->contentLen += chunkLen;

                    char *chunkData = strstr(chunkBegin, "\r\n");
                    if (!chunkData) assert(0);

                    chunkData += 2; // skip \r\n

                    memcpy(newCopyStart, chunkData, chunkLen); // copy & compute next copy location
                    newCopyStart += chunkLen;

                    assert(chunkData[chunkLen] == '\r' && chunkData[chunkLen + 1] == '\n'); // end of chunk
                    chunkBegin = chunkData + chunkLen + 2; // skip to new beginning
                }
                *newCopyStart = 0; // terminate string
                conn->readBuff.head = newCopyStart - conn->readBuff.buff + 1; // reclaim some buff space

                assert(conn->contentLen == newCopyStart - conn->content); // got all data

                conn->write_cb(conn->content, conn->contentLen, conn->extra_cb);
                conn->connectionStatus = CONN_DONE;

                ev_io_stop(loop, (struct ev_io *) &conn->ior);
                if(conn->secure) wolfSSL_free(conn->ssl);
            }
        } else if (strcmp(&conn->readBuff.buff[conn->readBuff.head - 1], "\n") == 0) {
            conn->write_cb(conn->content, 0, conn->extra_cb);
            conn->connectionStatus = CONN_DONE;

            ev_io_stop(loop, (struct ev_io *) &conn->ior);
            if(conn->secure) wolfSSL_free(conn->ssl);
        }


    } else if (conn->connectionStatus == CONN_TLS_HANDSHAKE) {
        int ret = wolfSSL_connect(conn->ssl);

        if (SNOW_UNLIKELY(ret != SSL_SUCCESS)) {
            char buffer[80];
            int err = wolfSSL_get_error(conn->ssl, ret);
            if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
                fprintf(stderr, "error = %d, %s\n", err, wolfSSL_ERR_error_string(err, buffer));
                assert(0);
            }
        } else {
            conn->connectionStatus = CONN_READY;
        }
    }
}

static void snow_io_write_cb(struct ev_loop *loop, struct ev_io *w, int revents) {
    auto *conn = (struct snow_connection_t *) ((struct ev_io_snow *) w)->data;

    if (conn->connectionStatus > CONN_READY) return;

    if (conn->connectionStatus == CONN_READY && !buff_empty(&conn->writeBuff)) {
        int rem = buff_pull_to_sock(&conn->writeBuff, conn->secure ? (void *) conn->ssl : (void *) &conn->sockfd, buff_to_pull(&conn->writeBuff),
                                    conn->secure);

        if (rem == 0) {
            conn->connectionStatus = CONN_WAITING;
            ev_io_stop(loop, (struct ev_io *) &conn->iow);
        }

        return;
    }

    if (conn->connectionStatus == CONN_IN_PROGRESS) {
        int conn_r = connect(conn->sockfd, (struct sockaddr *) &conn->address, sizeof(conn->address));
        if (conn_r == 0) {
            conn->connectionStatus = CONN_ACK;
            if (conn->secure)
                snow_startTLSHandshake(conn->global, conn);
            else conn->connectionStatus = CONN_READY;
        };
    }

    if (conn->connectionStatus == CONN_TLS_HANDSHAKE) {
        int ret = wolfSSL_connect(conn->ssl);

        if (SNOW_UNLIKELY(ret != SSL_SUCCESS)) {
            char buffer[80];
            int err = wolfSSL_get_error(conn->ssl, ret);
            if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
                fprintf(stderr, "error = %d, %s\n", err, wolfSSL_ERR_error_string(err, buffer));
                assert(0);
            }
        } else {
            conn->connectionStatus = CONN_READY;
        }
    }

}

void snow_initConnection(snow_connection_t *conn) {
    conn->sockfd = socket(conn->address.sin_family, SOCK_STREAM | SOCK_NONBLOCK, 0); // nonblocking sock
    conn->connectionStatus = CONN_UNREADY;

#ifdef DISABLE_NAGLE
    int flag = 1;
    setsockopt(conn->sockfd, IPPROTO_TCP, TCP_NODELAY, (char *) &flag, sizeof(int));
#endif

    ev_io_init((struct ev_io *) &conn->ior, snow_io_read_cb, conn->sockfd, EV_READ);
    ev_io_init((struct ev_io *) &conn->iow, snow_io_write_cb, conn->sockfd, EV_WRITE);
    conn->ior.data = conn;
    conn->iow.data = conn;

    static char str[1000];
    inet_ntop(AF_INET, &conn->address.sin_addr, str, INET_ADDRSTRLEN);

    if (SNOW_UNLIKELY(conn->sockfd == -1)) {
        std::cerr << "Socket creation failed\n";
        assert(0);
    }

    int conn_r = connect(conn->sockfd, (struct sockaddr *) &conn->address, sizeof(conn->address));
    conn->connectionStatus = CONN_IN_PROGRESS;

    if (SNOW_UNLIKELY(conn_r != 0 && errno != EINPROGRESS)) {
        std::cerr << "Socket connection failed: " << errno << "\n";
        assert(0);
    }

    ev_io_start(conn->global->loop, (struct ev_io *) &conn->ior);
    ev_io_start(conn->global->loop, (struct ev_io *) &conn->iow);
}

void snow_sendRequest(snow_connection_t *conn) {

    int size = sprintf(conn->writeBuff.buff, "%s /%s HTTP/1.1\r\nHost: %s\r\n\r\n",
                       method_strings[conn->method], conn->path, conn->hostname);

    conn->writeBuff.head += size;

}

void snow_do(snow_global_t *global, int method, const char *url, void (*write_cb)(char *data, size_t data_len, void *extra), void *extra) {
    int id = global->newConnId;
    snow_connection_t *conn = &global->connections[id];

    if (SNOW_UNLIKELY(conn->connectionStatus != CONN_UNREADY && conn->connectionStatus != CONN_DONE)) {
        std::cerr << "caught back to conn buff\n";
        assert(0);
    }

    memset(conn, 0, sizeof(snow_connection_t));

    conn->global = global;

    strcpy(conn->requestUrl, url);
    conn->method = method;
    conn->write_cb = write_cb;
    conn->extra_cb = extra;

    snow_parseUrl(conn);
    snow_resolveHost(conn);
    snow_initConnection(conn);
    snow_sendRequest(conn);

    global->newConnId++;
    global->newConnId %= concurrentConnections;
}

void snow_init(snow_global_t *global) {
    wolfSSL_Init();

    global->wolfCtx = wolfSSL_CTX_new(wolfTLSv1_2_client_method());

    if (global->wolfCtx == nullptr) {
        fprintf(stderr, "wolfSSL_CTX_new error.\n");
        assert(0);
    }

    // Load CA certificates into WOLFSSL_CTX
    if (wolfSSL_CTX_load_verify_locations(global->wolfCtx, "/etc/ssl/certs/ca-certificates.crt", nullptr) != SSL_SUCCESS) {
        fprintf(stderr, "Error loading /etc/ssl/certs/ca-certificates.crt");
        assert(0);
    }

}

void snow_destroy(snow_global_t *global) {
    wolfSSL_CTX_free(global->wolfCtx);
    wolfSSL_Cleanup();
}