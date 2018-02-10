/*
 * Copyright 2013 The Netty Project
 *
 * The Netty Project licenses this file to you under the Apache License,
 * version 2.0 (the "License"); you may not use this file except in compliance
 * with the License. You may obtain a copy of the License at:
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations
 * under the License.
 */
#define _GNU_SOURCE
#include <jni.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <linux/if_alg.h>
#include <sys/param.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/sendfile.h>
#include <sys/un.h>
#include <netinet/tcp.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/utsname.h>
#include <stddef.h>
#include <byteswap.h>
#include <dlfcn.h>
#include "io_netty_channel_epoll_Native.h"
#include "io_netty_channel_unix_FileDescriptor.h"
/**
 * On older Linux kernels, epoll can't handle timeout
 * values bigger than (LONG_MAX - 999ULL)/HZ.
 *
 * See:
 *   - https://github.com/libevent/libevent/blob/master/epoll.c#L138
 *   - http://cvs.schmorp.de/libev/ev_epoll.c?revision=1.68&view=markup
 */
#define MAX_EPOLL_TIMEOUT_MSEC (35*60*1000)

#ifndef SOL_ALG
#define SOL_ALG 279
#endif

// optional
extern int accept4(int sockFd, struct sockaddr* addr, socklen_t* addrlen, int flags) __attribute__((weak));
extern int epoll_create1(int flags) __attribute__((weak));

#ifdef IO_NETTY_SENDMMSG_NOT_FOUND
extern int sendmmsg(int sockfd, struct mmsghdr* msgvec, unsigned int vlen, unsigned int flags) __attribute__((weak));

#ifndef __USE_GNU
struct mmsghdr {
    struct msghdr msg_hdr;  /* Message header */
    unsigned int  msg_len;  /* Number of bytes transmitted */
};
#endif
#endif

// Those are initialized in the init(...) method and cached for performance reasons
jmethodID updatePosId = NULL;
jmethodID posId = NULL;
jmethodID limitId = NULL;
jfieldID posFieldId = NULL;
jfieldID limitFieldId = NULL;
jfieldID fileChannelFieldId = NULL;
jfieldID transferedFieldId = NULL;
jfieldID crc32FileChannelFieldId = NULL;
jfieldID crc32TransferedFieldId = NULL;
jfieldID fdFieldId = NULL;
jfieldID fileDescriptorFieldId = NULL;

jfieldID packetAddrFieldId = NULL;
jfieldID packetScopeIdFieldId = NULL;
jfieldID packetPortFieldId = NULL;
jfieldID packetMemoryAddressFieldId = NULL;
jfieldID packetCountFieldId = NULL;

jmethodID inetSocketAddrMethodId = NULL;
jmethodID datagramSocketAddrMethodId = NULL;
jclass runtimeExceptionClass = NULL;
jclass ioExceptionClass = NULL;
jclass closedChannelExceptionClass = NULL;
jmethodID closedChannelExceptionMethodId = NULL;
jclass inetSocketAddressClass = NULL;
jclass datagramSocketAddressClass = NULL;
jclass nativeDatagramPacketClass = NULL;
jclass netUtilClass = NULL;
jmethodID netUtilClassIpv4PreferredMethodId = NULL;

static int socketType;
static const char* ip4prefix = "::ffff:";

// util methods
static inline void throwRuntimeException(JNIEnv* env, char* message) {
    (*env)->ThrowNew(env, runtimeExceptionClass, message);
}

static inline void throwIOException(JNIEnv* env, char* message) {
    (*env)->ThrowNew(env, ioExceptionClass, message);
}

static inline void throwClosedChannelException(JNIEnv* env) {
    jobject exception = (*env)->NewObject(env, closedChannelExceptionClass, closedChannelExceptionMethodId);
    (*env)->Throw(env, exception);
}

static inline void throwOutOfMemoryError(JNIEnv* env) {
    jclass exceptionClass = (*env)->FindClass(env, "java/lang/OutOfMemoryError");
    (*env)->ThrowNew(env, exceptionClass, "");
}

static inline char* exceptionMessage(char* msg, int error) {
    char* err = strerror(error);
    char* result = malloc(strlen(msg) + strlen(err) + 1);
    strcpy(result, msg);
    strcat(result, err);
    return result;
}

static int getSysctlValue(const char * property, int* returnValue) {
    int rc = -1;
    FILE *fd=fopen(property, "r");
    if (fd != NULL) {
      char buf[32] = {0x0};
      if (fgets(buf, 32, fd) != NULL) {
        *returnValue = atoi(buf);
        rc = 0;
      }
      fclose(fd);
    }
    return rc;
}

static inline jint epollCtl(JNIEnv* env, jint efd, int op, jint fd, jint flags) {
    uint32_t events = flags;
    struct epoll_event ev = {
        .data.fd = fd,
        .events = events
    };

    return epoll_ctl(efd, op, fd, &ev);
}

static inline jint getOption(JNIEnv* env, jint fd, int level, int optname, void* optval, socklen_t optlen) {
    int code;
    code = getsockopt(fd, level, optname, optval, &optlen);
    if (code == 0) {
        return 0;
    }
    int err = errno;
    throwRuntimeException(env, exceptionMessage("getsockopt() failed: ", err));
    return code;
}

static inline int setOption(JNIEnv* env, jint fd, int level, int optname, const void* optval, socklen_t len) {
    int rc = setsockopt(fd, level, optname, optval, len);
    if (rc < 0) {
        int err = errno;
        throwRuntimeException(env, exceptionMessage("setsockopt() failed: ", err));
    }
    return rc;
}

jobject createInetSocketAddress(JNIEnv* env, const struct sockaddr_storage* addr) {
    char ipstr[INET6_ADDRSTRLEN];
    int port;
    jstring ipString;
    if (addr->ss_family == AF_INET) {
        struct sockaddr_in* s = (struct sockaddr_in*) addr;
        port = ntohs(s->sin_port);
        inet_ntop(AF_INET, &s->sin_addr, ipstr, sizeof ipstr);
        ipString = (*env)->NewStringUTF(env, ipstr);
    } else {
        struct sockaddr_in6* s = (struct sockaddr_in6*) addr;
        port = ntohs(s->sin6_port);
        inet_ntop(AF_INET6, &s->sin6_addr, ipstr, sizeof ipstr);
        if (strncasecmp(ipstr, ip4prefix, 7) == 0) {
            // IPv4-mapped-on-IPv6.
            // Cut of ::ffff: prefix to workaround performance issues when parsing these
            // addresses in InetAddress.getByName(...).
            //
            // See https://github.com/netty/netty/issues/2867
            ipString = (*env)->NewStringUTF(env, &ipstr[7]);
        } else {
            ipString = (*env)->NewStringUTF(env, ipstr);
        }
    }
    jobject socketAddr = (*env)->NewObject(env, inetSocketAddressClass, inetSocketAddrMethodId, ipString, port);
    return socketAddr;
}


static inline int addressLength(const struct sockaddr_storage* addr) {
    if (addr->ss_family == AF_INET) {
        return 8;
    } else {
        struct sockaddr_in6* s = (struct sockaddr_in6*) addr;
        if (s->sin6_addr.s6_addr[0] == 0x00 && s->sin6_addr.s6_addr[1] == 0x00 && s->sin6_addr.s6_addr[2] == 0x00 && s->sin6_addr.s6_addr[3] == 0x00 && s->sin6_addr.s6_addr[4] == 0x00
                && s->sin6_addr.s6_addr[5] == 0x00 && s->sin6_addr.s6_addr[6] == 0x00 && s->sin6_addr.s6_addr[7] == 0x00 && s->sin6_addr.s6_addr[8] == 0x00 && s->sin6_addr.s6_addr[9] == 0x00
                 && s->sin6_addr.s6_addr[10] == 0xff && s->sin6_addr.s6_addr[11] == 0xff) {
            // IPv4-mapped-on-IPv6
            return 8;
        } else {
            return 24;
        }
    }
}

static inline void initInetSocketAddressArray(JNIEnv* env, const struct sockaddr_storage* addr, jbyteArray bArray, int offset, int len) {
    int port;
    if (addr->ss_family == AF_INET) {
        struct sockaddr_in* s = (struct sockaddr_in*) addr;
        port = ntohs(s->sin_port);

        // Encode address and port into the array
        unsigned char a[4];
        a[0] = port >> 24;
        a[1] = port >> 16;
        a[2] = port >> 8;
        a[3] = port;
        (*env)->SetByteArrayRegion(env, bArray, offset, 4, (jbyte*) &s->sin_addr.s_addr);
        (*env)->SetByteArrayRegion(env, bArray, offset + 4, 4, (jbyte*) &a);
    } else {
        struct sockaddr_in6* s = (struct sockaddr_in6*) addr;
        port = ntohs(s->sin6_port);

        if (len == 8) {
            // IPv4-mapped-on-IPv6
            // Encode port into the array and write it into the jbyteArray
            unsigned char a[4];
            a[0] = port >> 24;
            a[1] = port >> 16;
            a[2] = port >> 8;
            a[3] = port;

            // we only need the last 4 bytes for mapped address
            (*env)->SetByteArrayRegion(env, bArray, offset, 4, (jbyte*) &(s->sin6_addr.s6_addr[12]));
            (*env)->SetByteArrayRegion(env, bArray, offset + 4, 4, (jbyte*) &a);
        } else {
            // Encode scopeid and port into the array
            unsigned char a[8];
            a[0] = s->sin6_scope_id >> 24;
            a[1] = s->sin6_scope_id >> 16;
            a[2] = s->sin6_scope_id >> 8;
            a[3] = s->sin6_scope_id;
            a[4] = port >> 24;
            a[5] = port >> 16;
            a[6] = port >> 8;
            a[7] = port;

            (*env)->SetByteArrayRegion(env, bArray, offset, 16, (jbyte*) &(s->sin6_addr.s6_addr));
            (*env)->SetByteArrayRegion(env, bArray, offset + 16, 8, (jbyte*) &a);
        }
    }
}

static jbyteArray createInetSocketAddressArray(JNIEnv* env, const struct sockaddr_storage* addr) {
    int len = addressLength(addr);
    jbyteArray bArray = (*env)->NewByteArray(env, len);

    initInetSocketAddressArray(env, addr, bArray, 0, len);
    return bArray;
}

static jobject createDatagramSocketAddress(JNIEnv* env, const struct sockaddr_storage* addr, int len) {
    char ipstr[INET6_ADDRSTRLEN];
    int port;
    jstring ipString;
    if (addr->ss_family == AF_INET) {
        struct sockaddr_in* s = (struct sockaddr_in*) addr;
        port = ntohs(s->sin_port);
        inet_ntop(AF_INET, &s->sin_addr, ipstr, sizeof ipstr);
        ipString = (*env)->NewStringUTF(env, ipstr);
    } else {
        struct sockaddr_in6* s = (struct sockaddr_in6*) addr;
        port = ntohs(s->sin6_port);
        inet_ntop(AF_INET6, &s->sin6_addr, ipstr, sizeof ipstr);

        if (strncasecmp(ipstr, ip4prefix, 7) == 0) {
            // IPv4-mapped-on-IPv6.
            // Cut of ::ffff: prefix to workaround performance issues when parsing these
            // addresses in InetAddress.getByName(...).
            //
            // See https://github.com/netty/netty/issues/2867
            ipString = (*env)->NewStringUTF(env, &ipstr[7]);
        } else {
            ipString = (*env)->NewStringUTF(env, ipstr);
        }
    }
    jobject socketAddr = (*env)->NewObject(env, datagramSocketAddressClass, datagramSocketAddrMethodId, ipString, port, len);
    return socketAddr;
}

static int init_sockaddr(JNIEnv* env, jbyteArray address, jint scopeId, jint jport, const struct sockaddr_storage* addr) {
    uint16_t port = htons((uint16_t) jport);
    // Use GetPrimitiveArrayCritical and ReleasePrimitiveArrayCritical to signal the VM that we really would like
    // to not do a memory copy here. This is ok as we not do any blocking action here anyway.
    // This is important as the VM may suspend GC for the time!
    jbyte* addressBytes = (*env)->GetPrimitiveArrayCritical(env, address, 0);
    if (addressBytes == NULL) {
        // No memory left ?!?!?
        throwOutOfMemoryError(env);
    }
    if (socketType == AF_INET6) {
        struct sockaddr_in6* ip6addr = (struct sockaddr_in6*) addr;
        ip6addr->sin6_family = AF_INET6;
        ip6addr->sin6_port = port;

        if (scopeId != 0) {
           ip6addr->sin6_scope_id = (uint32_t) scopeId;
        }
        memcpy(&(ip6addr->sin6_addr.s6_addr), addressBytes, 16);
    } else {
        struct sockaddr_in* ipaddr = (struct sockaddr_in*) addr;
        ipaddr->sin_family = AF_INET;
        ipaddr->sin_port = port;
        memcpy(&(ipaddr->sin_addr.s_addr), addressBytes + 12, 4);
    }

    (*env)->ReleasePrimitiveArrayCritical(env, address, addressBytes, JNI_ABORT);
    return 0;
}

static int socket_type(JNIEnv* env) {
    jboolean ipv4Preferred = (*env)->CallStaticBooleanMethod(env, netUtilClass, netUtilClassIpv4PreferredMethodId);

    if (ipv4Preferred) {
        // User asked to use ipv4 explicitly.
        return AF_INET;
    }
    int fd = socket(AF_INET6, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd == -1) {
        if (errno == EAFNOSUPPORT) {
            return AF_INET;
        }
        return AF_INET6;
    } else {
        close(fd);
        return AF_INET6;
    }
}

static int init_in_addr(JNIEnv* env, jbyteArray address, struct in_addr* addr) {
    // Use GetPrimitiveArrayCritical and ReleasePrimitiveArrayCritical to signal the VM that we really would like
    // to not do a memory copy here. This is ok as we not do any blocking action here anyway.
    // This is important as the VM may suspend GC for the time!
    jbyte* addressBytes = (*env)->GetPrimitiveArrayCritical(env, address, 0);
    if (addressBytes == NULL) {
        // No memory left ?!?!?
        throwOutOfMemoryError(env);
        return -1;
    }
    if (socketType == AF_INET6) {
        memcpy(addr, addressBytes, 16);
    } else {
        memcpy(addr, addressBytes + 12, 4);
    }
    (*env)->ReleasePrimitiveArrayCritical(env, address, addressBytes, JNI_ABORT);
    return 0;
}
// util methods end

static jint netty_epoll_native_eventFd(JNIEnv* env, jclass clazz) {
    jint eventFD =  eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);

    if (eventFD < 0) {
        int err = errno;
        throwRuntimeException(env, exceptionMessage("eventfd() failed: ", err));
    }
    return eventFD;
}

static void netty_epoll_native_eventFdWrite(JNIEnv* env, jclass clazz, jint fd, jlong value) {
    jint eventFD = eventfd_write(fd, (eventfd_t) value);

    if (eventFD < 0) {
        int err = errno;
        throwRuntimeException(env, exceptionMessage("eventfd_write() failed: ", err));
    }
}

static void netty_epoll_native_eventFdRead(JNIEnv* env, jclass clazz, jint fd) {
    uint64_t eventfd_t;

    if (eventfd_read(fd, &eventfd_t) != 0) {
        // something is serious wrong
        throwRuntimeException(env, "eventfd_read() failed");
    }
}

static jint netty_epoll_native_epollCreate(JNIEnv* env, jclass clazz) {
    jint efd;
    if (epoll_create1) {
        efd = epoll_create1(EPOLL_CLOEXEC);
    } else {
        // size will be ignored anyway but must be positive
        efd = epoll_create(126);
    }
    if (efd < 0) {
        int err = errno;
        if (epoll_create1) {
            throwRuntimeException(env, exceptionMessage("epoll_create1() failed: ", err));
        } else {
            throwRuntimeException(env, exceptionMessage("epoll_create() failed: ", err));
        }
        return efd;
    }
    if (!epoll_create1) {
        if (fcntl(efd, F_SETFD, FD_CLOEXEC) < 0) {
            int err = errno;
            close(efd);
            throwRuntimeException(env, exceptionMessage("fcntl() failed: ", err));
            return err;
        }
    }
    return efd;
}

static jint netty_epoll_native_epollWait0(JNIEnv* env, jclass clazz, jint efd, jlong address, jint len, jint timeout) {
    struct epoll_event *ev = (struct epoll_event*) address;
    int ready;
    int err;

    if (timeout > MAX_EPOLL_TIMEOUT_MSEC) {
        // Workaround for bug in older linux kernels that can not handle bigger timeout then MAX_EPOLL_TIMEOUT_MSEC.
        timeout = MAX_EPOLL_TIMEOUT_MSEC;
    }

    do {
       ready = epoll_wait(efd, ev, len, timeout);
       // was interrupted try again.
    } while (ready == -1 && ((err = errno) == EINTR));

    if (ready < 0) {
         return -err;
    }
    return ready;
}

static void netty_epoll_native_epollCtlAdd0(JNIEnv* env, jclass clazz, jint efd, jint fd, jint flags) {
    if (epollCtl(env, efd, EPOLL_CTL_ADD, fd, flags) < 0) {
        int err = errno;
        throwRuntimeException(env, exceptionMessage("epoll_ctl() failed: ", err));
    }
}

static void netty_epoll_native_epollCtlMod0(JNIEnv* env, jclass clazz, jint efd, jint fd, jint flags) {
    if (epollCtl(env, efd, EPOLL_CTL_MOD, fd, flags) < 0) {
        int err = errno;
        throwRuntimeException(env, exceptionMessage("epoll_ctl() failed: ", err));
    }
}

static void netty_epoll_native_epollCtlDel0(JNIEnv* env, jclass clazz, jint efd, jint fd) {
    // Create an empty event to workaround a bug in older kernels which can not handle NULL.
    struct epoll_event event = { 0 };
    if (epoll_ctl(efd, EPOLL_CTL_DEL, fd, &event) < 0) {
        int err = errno;
        throwRuntimeException(env, exceptionMessage("epoll_ctl() failed: ", err));
    }
}

static inline jint _write(JNIEnv* env, jclass clazz, jint fd, void* buffer, jint pos, jint limit) {
    ssize_t res;
    int err;
    do {
       res = write(fd, buffer + pos, (size_t) (limit - pos));
       // keep on writing if it was interrupted
    } while (res == -1 && ((err = errno) == EINTR));

    if (res < 0) {
        return -err;
    }
    return (jint) res;
}

static jint netty_epoll_native_write0(JNIEnv* env, jclass clazz, jint fd, jobject jbuffer, jint pos, jint limit) {
    void* buffer = (*env)->GetDirectBufferAddress(env, jbuffer);
    if (buffer == NULL) {
        throwRuntimeException(env, "failed to get direct buffer address");
        return -1;
    }
    return _write(env, clazz, fd, buffer, pos, limit);
}

static jint netty_epoll_native_writeAddress0(JNIEnv* env, jclass clazz, jint fd, jlong address, jint pos, jint limit) {
    return _write(env, clazz, fd, (void*) address, pos, limit);
}

static inline jint _sendTo(JNIEnv* env, jint fd, void* buffer, jint pos, jint limit, jbyteArray address, jint scopeId, jint port) {
    struct sockaddr_storage addr;
    if (init_sockaddr(env, address, scopeId, port, &addr) == -1) {
        return -1;
    }

    ssize_t res;
    int err;
    do {
       res = sendto(fd, buffer + pos, (size_t) (limit - pos), 0, (struct sockaddr*) &addr, sizeof(struct sockaddr_storage));
       // keep on writing if it was interrupted
    } while (res == -1 && ((err = errno) == EINTR));

    if (res < 0) {
        return -err;
    }
    return (jint) res;
}

static jint netty_epoll_native_sendTo0(JNIEnv* env, jclass clazz, jint fd, jobject jbuffer, jint pos, jint limit, jbyteArray address, jint scopeId, jint port) {
    void* buffer = (*env)->GetDirectBufferAddress(env, jbuffer);
    if (buffer == NULL) {
        throwRuntimeException(env, "failed to get direct buffer address");
        return -1;
    }
    return _sendTo(env, fd, buffer, pos, limit, address, scopeId, port);
}

static jint netty_epoll_native_sendToAddress0(JNIEnv* env, jclass clazz, jint fd, jlong memoryAddress, jint pos, jint limit ,jbyteArray address, jint scopeId, jint port) {
    return _sendTo(env, fd, (void*) memoryAddress, pos, limit, address, scopeId, port);
}

static jint netty_epoll_native_sendToAddresses(JNIEnv* env, jclass clazz, jint fd, jlong memoryAddress, jint length, jbyteArray address, jint scopeId, jint port) {
    struct sockaddr_storage addr;

    if (init_sockaddr(env, address, scopeId, port, &addr) == -1) {
        return -1;
    }

    struct msghdr m;
    m.msg_name = (void*) &addr;
    m.msg_namelen = (socklen_t) sizeof(struct sockaddr_storage);
    m.msg_iov = (struct iovec*) memoryAddress;
    m.msg_iovlen = length;

    ssize_t res;
    int err;
    do {
       res = sendmsg(fd, &m, 0);
       // keep on writing if it was interrupted
    } while (res == -1 && ((err = errno) == EINTR));

    if (res < 0) {
        return -err;
    }
    return (jint) res;
}

static jint netty_epoll_native_sendmmsg0(JNIEnv* env, jclass clazz, jint fd, jobjectArray packets, jint offset, jint len) {
    struct mmsghdr msg[len];
    int i;

    memset(msg, 0, sizeof(msg));

    for (i = 0; i < len; i++) {
        struct sockaddr_storage addr;

        jobject packet = (*env)->GetObjectArrayElement(env, packets, i + offset);
        jbyteArray address = (jbyteArray) (*env)->GetObjectField(env, packet, packetAddrFieldId);
        jint scopeId = (*env)->GetIntField(env, packet, packetScopeIdFieldId);
        jint port = (*env)->GetIntField(env, packet, packetPortFieldId);

        if (init_sockaddr(env, address, scopeId, port, &addr) == -1) {
            return -1;
        }

        msg[i].msg_hdr.msg_name = &addr;
        msg[i].msg_hdr.msg_namelen = sizeof(addr);

        msg[i].msg_hdr.msg_iov = (struct iovec*) (*env)->GetLongField(env, packet, packetMemoryAddressFieldId);
        msg[i].msg_hdr.msg_iovlen = (*env)->GetIntField(env, packet, packetCountFieldId);;
    }

    ssize_t res;
    int err;
    do {
       res = sendmmsg(fd, msg, len, 0);
       // keep on writing if it was interrupted
    } while (res == -1 && ((err = errno) == EINTR));

    if (res < 0) {
        return -err;
    }
    return (jint) res;
}

static inline jobject recvFrom0(JNIEnv* env, jint fd, void* buffer, jint pos, jint limit) {
    struct sockaddr_storage addr;
    socklen_t addrlen = sizeof(addr);
    ssize_t res;
    int err;

    do {
        res = recvfrom(fd, buffer + pos, (size_t) (limit - pos), 0, (struct sockaddr*) &addr, &addrlen);
        // Keep on reading if we was interrupted
    } while (res == -1 && ((err = errno) == EINTR));

    if (res < 0) {
        if (err == EAGAIN || err == EWOULDBLOCK) {
            // Nothing left to read
            return NULL;
        }
        if (err == EBADF) {
            throwClosedChannelException(env);
            return NULL;
        }
        throwIOException(env, exceptionMessage("recvfrom() failed: ", err));
        return NULL;
    }

    return createDatagramSocketAddress(env, &addr, res);
}

static jobject netty_epoll_native_recvFrom(JNIEnv* env, jclass clazz, jint fd, jobject jbuffer, jint pos, jint limit) {
    void* buffer = (*env)->GetDirectBufferAddress(env, jbuffer);
    if (buffer == NULL) {
        throwRuntimeException(env, "failed to get direct buffer address");
        return NULL;
    }

    return recvFrom0(env, fd, buffer, pos, limit);
}

static jobject netty_epoll_native_recvFromAddress(JNIEnv* env, jclass clazz, jint fd, jlong address, jint pos, jint limit) {
    return recvFrom0(env, fd, (void*) address, pos, limit);
}

static inline jlong _writev(JNIEnv* env, jclass clazz, jint fd, struct iovec* iov, jint length) {
    ssize_t res;
    int err;
    do {
        res = writev(fd, iov, length);
        // keep on writing if it was interrupted
    } while (res == -1 && ((err = errno) == EINTR));

    if (res < 0) {
        return -err;
    }
    return (jlong) res;
}

static jlong netty_epoll_native_writev0(JNIEnv* env, jclass clazz, jint fd, jobjectArray buffers, jint offset, jint length) {
    struct iovec iov[length];
    int iovidx = 0;
    int i;
    int num = offset + length;
    for (i = offset; i < num; i++) {
        jobject bufObj = (*env)->GetObjectArrayElement(env, buffers, i);
        jint pos;
        // Get the current position using the (*env)->GetIntField if possible and fallback
        // to slower (*env)->CallIntMethod(...) if needed
        if (posFieldId == NULL) {
            pos = (*env)->CallIntMethod(env, bufObj, posId, NULL);
        } else {
            pos = (*env)->GetIntField(env, bufObj, posFieldId);
        }
        jint limit;
        // Get the current limit using the (*env)->GetIntField if possible and fallback
        // to slower (*env)->CallIntMethod(...) if needed
        if (limitFieldId == NULL) {
            limit = (*env)->CallIntMethod(env, bufObj, limitId, NULL);
        } else {
            limit = (*env)->GetIntField(env, bufObj, limitFieldId);
        }
        void* buffer = (*env)->GetDirectBufferAddress(env, bufObj);
        if (buffer == NULL) {
            throwRuntimeException(env, "failed to get direct buffer address");
            return -1;
        }
        iov[iovidx].iov_base = buffer + pos;
        iov[iovidx].iov_len = (size_t) (limit - pos);
        iovidx++;

        // Explicit delete local reference as otherwise the local references will only be released once the native method returns.
        // Also there may be a lot of these and JNI specification only specify that 16 must be able to be created.
        //
        // See https://github.com/netty/netty/issues/2623
        (*env)->DeleteLocalRef(env, bufObj);
    }
    return _writev(env, clazz, fd, iov, length);
}

static jlong netty_epoll_native_writevAddresses0(JNIEnv* env, jclass clazz, jint fd, jlong memoryAddress, jint length) {
    struct iovec* iov = (struct iovec*) memoryAddress;
    return _writev(env, clazz, fd, iov, length);
}

static inline jint _read(JNIEnv* env, jclass clazz, jint fd, void* buffer, jint pos, jint limit) {
    ssize_t res;
    int err;
    do {
        res = read(fd, buffer + pos, (size_t) (limit - pos));
        // Keep on reading if we was interrupted
    } while (res == -1 && ((err = errno) == EINTR));

    if (res < 0) {
        return -err;
    }
    return (jint) res;
}

static jint netty_epoll_native_read0(JNIEnv* env, jclass clazz, jint fd, jobject jbuffer, jint pos, jint limit) {
    void* buffer = (*env)->GetDirectBufferAddress(env, jbuffer);
    if (buffer == NULL) {
        throwRuntimeException(env, "failed to get direct buffer address");
        return -1;
    }
    return _read(env, clazz, fd, buffer, pos, limit);
}

static jint netty_epoll_native_readAddress0(JNIEnv* env, jclass clazz, jint fd, jlong address, jint pos, jint limit) {
    return _read(env, clazz, fd, (void*) address, pos, limit);
}

static jint netty_epoll_native_close0(JNIEnv* env, jclass clazz, jint fd) {
   if (close(fd) < 0) {
       return -errno;
   }
   return 0;
}

static jint netty_epoll_native_shutdown0(JNIEnv* env, jclass clazz, jint fd, jboolean read, jboolean write) {
    int mode;
    if (read && write) {
        mode = SHUT_RDWR;
    } else if (read) {
        mode = SHUT_RD;
    } else if (write) {
        mode = SHUT_WR;
    }
    if (shutdown(fd, mode) < 0) {
        return -errno;
    }
    return 0;
}

static inline jint socket0(JNIEnv* env, jclass clazz, int type) {
    int fd = socket(socketType, type | SOCK_NONBLOCK, 0);
    if (fd == -1) {
        return -errno;
    } else if (socketType == AF_INET6) {
        // Allow to listen /connect ipv4 and ipv6
        int optval = 0;
        if (setOption(env, fd, IPPROTO_IPV6, IPV6_V6ONLY, &optval, sizeof(optval)) < 0) {
            // Something went wrong so close the fd and return here. setOption(...) itself throws the exception already.
            close(fd);
            return -1;
        }
    }
    return fd;
}

static jint netty_epoll_native_socketDgram(JNIEnv* env, jclass clazz) {
    return socket0(env, clazz, SOCK_DGRAM);
}

static jint netty_epoll_native_socketStream(JNIEnv* env, jclass clazz) {
    return socket0(env, clazz, SOCK_STREAM);
}

static jint netty_epoll_native_bind(JNIEnv* env, jclass clazz, jint fd, jbyteArray address, jint scopeId, jint port) {
    struct sockaddr_storage addr;
    if (init_sockaddr(env, address, scopeId, port, &addr) == -1) {
        return -1;
    }

    if (bind(fd, (struct sockaddr*) &addr, sizeof(addr)) == -1) {
        return -errno;
    }

    return 0;
}

static jint netty_epoll_native_listen0(JNIEnv* env, jclass clazz, jint fd, jint backlog) {
    if (listen(fd, backlog) == -1) {
        return -errno;
    }
    return 0;
}

static jint netty_epoll_native_connect(JNIEnv* env, jclass clazz, jint fd, jbyteArray address, jint scopeId, jint port) {
    struct sockaddr_storage addr;
    if (init_sockaddr(env, address, scopeId, port, &addr) == -1) {
        // A runtime exception was thrown
        return -1;
    }

    int res;
    int err;
    do {
        res = connect(fd, (struct sockaddr*) &addr, sizeof(addr));
    } while (res == -1 && ((err = errno) == EINTR));

    if (res < 0) {
        return -err;
    }
    return 0;
}

static jint netty_epoll_native_finishConnect0(JNIEnv* env, jclass clazz, jint fd) {
    // connect may be done
    // return true if connection finished successfully
    // return false if connection is still in progress
    // throw exception if connection failed
    int optval;
    int res = getOption(env, fd, SOL_SOCKET, SO_ERROR, &optval, sizeof(optval));
    if (res != 0) {
        // getOption failed
        return -1;
    }
    if (optval == 0) {
        // connect succeeded
        return 0;
    }
    return -optval;
}

static jint netty_epoll_native_accept0(JNIEnv* env, jclass clazz, jint fd, jbyteArray acceptedAddress) {
    jint socketFd;
    int err;
    struct sockaddr_storage addr;
    socklen_t address_len = sizeof(addr);

    do {
        if (accept4) {
            socketFd = accept4(fd, (struct sockaddr*) &addr, &address_len, SOCK_NONBLOCK | SOCK_CLOEXEC);
        } else  {
            socketFd = accept(fd, (struct sockaddr*) &addr, &address_len);
        }
    } while (socketFd == -1 && ((err = errno) == EINTR));

    if (socketFd == -1) {
        return -err;
    }

    int len = addressLength(&addr);

    // Fill in remote address details
    (*env)->SetByteArrayRegion(env, acceptedAddress, 0, 4, (jbyte*) &len);
    initInetSocketAddressArray(env, &addr, acceptedAddress, 1, len);

    if (accept4)  {
        return socketFd;
    } else  {
        // accept4 was not present so need two more sys-calls ...
        if (fcntl(socketFd, F_SETFD, FD_CLOEXEC) == -1) {
            return -errno;
        }
        if (fcntl(socketFd, F_SETFL, O_NONBLOCK) == -1) {
            return -errno;
        }
    }
    return socketFd;
}

static jlong netty_epoll_native_sendfile0(JNIEnv* env, jclass clazz, jint fd, jobject fileRegion, jlong base_off, jlong off, jlong len) {
    jobject fileChannel = (*env)->GetObjectField(env, fileRegion, fileChannelFieldId);
    if (fileChannel == NULL) {
        throwRuntimeException(env, "failed to get DefaultFileRegion.file");
        return -1;
    }
    jobject fileDescriptor = (*env)->GetObjectField(env, fileChannel, fileDescriptorFieldId);
    if (fileDescriptor == NULL) {
        throwRuntimeException(env, "failed to get FileChannelImpl.fd");
        return -1;
    }
    jint srcFd = (*env)->GetIntField(env, fileDescriptor, fdFieldId);
    if (srcFd == -1) {
        throwRuntimeException(env, "failed to get FileDescriptor.fd");
        return -1;
    }
    ssize_t res;
    off_t offset = base_off + off;
    int err;
    do {
      res = sendfile(fd, srcFd, &offset, (size_t) len);
    } while (res == -1 && ((err = errno) == EINTR));
    if (res < 0) {
        return -err;
    }
    if (res > 0) {
        // update the transfered field in DefaultFileRegion
        (*env)->SetLongField(env, fileRegion, transferedFieldId, off + res);
    }

    return res;
}

static jint netty_epoll_native_create_crc32_socket(JNIEnv* env, jclass clazz) {

    int sock_fd = -1;

    struct sockaddr_alg sa = {
        .salg_family = AF_ALG,
        .salg_type   = "hash",
        .salg_name   = "crc32c"
    };

    if ((sock_fd = socket(AF_ALG, SOCK_SEQPACKET, 0)) == -1) {
        throwRuntimeException(env, "failed to retrieve socket for alg fd");
        return -1;
    }

    if (bind(sock_fd, (struct sockaddr *) &sa, sizeof(sa)) != 0) {
        int err = errno;
        throwRuntimeException(env, exceptionMessage("failed to bind to socket alg: ", err));
        return -1;
    }

    int crc = ~0;
    if (setsockopt(sock_fd, SOL_ALG, ALG_SET_KEY, &crc, 4) != 0) {
        throwRuntimeException(env, "failed to setsockopt for alg fd");
        return -1;
    }

    return sock_fd;
}

static jint netty_epoll_native_connect_crc32_socket(JNIEnv* env, jclass clazz, jint server_fd) {
    int fd_client = -1;
    if ((fd_client = accept(server_fd, NULL, 0)) == -1) {
        int err = errno;
        throwRuntimeException(env, exceptionMessage("failed to connect to alg fd: ", errno));
        return -1;
    }
    return fd_client;
}

static jlong netty_epoll_native_sendfile_crc32(JNIEnv* env, jclass clazz, jint fd,
    jobject fileRegion, jlong base_off, jlong off, jlong len, jint crc32_client) {
    jobject fileChannel = (*env)->GetObjectField(env, fileRegion, crc32FileChannelFieldId);
    if (fileChannel == NULL) {
        throwRuntimeException(env, "failed to get CRC32FileRegion.file");
        return -1;
    }
    jobject fileDescriptor = (*env)->GetObjectField(env, fileChannel, fileDescriptorFieldId);
    if (fileDescriptor == NULL) {
        throwRuntimeException(env, "failed to get FileChannelImpl.fd");
        return -1;
    }
    jint srcFd = (*env)->GetIntField(env, fileDescriptor, fdFieldId);
    if (srcFd == -1) {
        throwRuntimeException(env, "failed to get FileDescriptor.fd");
        return -1;
    }
    ssize_t res, res0;
    off_t offset = base_off + off;
    int err;

    off_t ofs = offset;
    uint32_t crc32c = 0;
    //do {
      res0 = sendfile(crc32_client, srcFd, &ofs, (size_t) len);
      read(crc32_client, &crc32c, sizeof(uint32_t));
      crc32c = ~__bswap_32(~crc32c); // linux kernel leaves this to the user...
      int crc_sent = send(fd, &crc32c, sizeof(uint32_t), MSG_MORE);
    //} while (res0 == -1 && ((err = errno) == EINTR));

    do {
      res = sendfile(fd, srcFd, &offset, (size_t) len);
    } while (res == -1 && ((err = errno) == EINTR));
    if (res < 0) {
        return -err;
    }
    if (res > 0) {
        // update the transfered field in DefaultFileRegion
        (*env)->SetLongField(env, fileRegion, crc32TransferedFieldId, off + res);
    }
    return res + crc_sent;
}

static jbyteArray netty_epoll_native_remoteAddress0(JNIEnv* env, jclass clazz, jint fd) {
    socklen_t len;
    struct sockaddr_storage addr;

    len = sizeof addr;
    if (getpeername(fd, (struct sockaddr*) &addr, &len) == -1) {
        return NULL;
    }
    return createInetSocketAddressArray(env, &addr);
}

static jbyteArray netty_epoll_native_localAddress0(JNIEnv* env, jclass clazz, jint fd) {
    socklen_t len;
    struct sockaddr_storage addr;

    len = sizeof addr;
    if (getsockname(fd, (struct sockaddr*) &addr, &len) == -1) {
        return NULL;
    }
    return createInetSocketAddressArray(env, &addr);
}

static void netty_epoll_native_setReuseAddress(JNIEnv* env, jclass clazz, jint fd, jint optval) {
    setOption(env, fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
}

static void netty_epoll_native_setReusePort(JNIEnv* env, jclass clazz, jint fd, jint optval) {
    setOption(env, fd, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof(optval));
}

static void netty_epoll_native_setTcpNoDelay(JNIEnv* env, jclass clazz, jint fd, jint optval) {
    setOption(env, fd, IPPROTO_TCP, TCP_NODELAY, &optval, sizeof(optval));
}

static void netty_epoll_native_setReceiveBufferSize(JNIEnv* env, jclass clazz, jint fd, jint optval) {
    setOption(env, fd, SOL_SOCKET, SO_RCVBUF, &optval, sizeof(optval));
}

static void netty_epoll_native_setSendBufferSize(JNIEnv* env, jclass clazz, jint fd, jint optval) {
    setOption(env, fd, SOL_SOCKET, SO_SNDBUF, &optval, sizeof(optval));
}

static void netty_epoll_native_setKeepAlive(JNIEnv* env, jclass clazz, jint fd, jint optval) {
    setOption(env, fd, SOL_SOCKET, SO_KEEPALIVE, &optval, sizeof(optval));
}

static void netty_epoll_native_setTcpCork(JNIEnv* env, jclass clazz, jint fd, jint optval) {
    setOption(env, fd, SOL_TCP, TCP_CORK, &optval, sizeof(optval));
}

static void netty_epoll_native_setSoLinger(JNIEnv* env, jclass clazz, jint fd, jint optval) {
    setOption(env, fd, IPPROTO_IP, IP_TOS, &optval, sizeof(optval));
}

static void netty_epoll_native_setTrafficClass(JNIEnv* env, jclass clazz, jint fd, jint optval) {
    struct linger solinger;
    if (optval < 0) {
        solinger.l_onoff = 0;
        solinger.l_linger = 0;
    } else {
        solinger.l_onoff = 1;
        solinger.l_linger = optval;
    }
    setOption(env, fd, SOL_SOCKET, SO_LINGER, &solinger, sizeof(solinger));
}

static void netty_epoll_native_setBroadcast(JNIEnv* env, jclass clazz, jint fd, jint optval) {
    setOption(env, fd, SOL_SOCKET, SO_BROADCAST, &optval, sizeof(optval));
}

static void netty_epoll_native_setTcpKeepIdle(JNIEnv* env, jclass clazz, jint fd, jint optval) {
    setOption(env, fd, SOL_TCP, TCP_KEEPIDLE, &optval, sizeof(optval));
}

static void netty_epoll_native_setTcpKeepIntvl(JNIEnv* env, jclass clazz, jint fd, jint optval) {
    setOption(env, fd, SOL_TCP, TCP_KEEPINTVL, &optval, sizeof(optval));
}

static voidnetty_epoll_native_setTcpKeepCnt(JNIEnv* env, jclass clazz, jint fd, jint optval) {
    setOption(env, fd, SOL_TCP, TCP_KEEPCNT, &optval, sizeof(optval));
}

static jint netty_epoll_native_isReuseAddress(JNIEnv* env, jclass clazz, jint fd) {
    int optval;
    if (getOption(env, fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) == -1) {
        return -1;
    }
    return optval;
}

static jint netty_epoll_native_isReusePort(JNIEnv* env, jclass clazz, jint fd) {
    int optval;
    if (getOption(env, fd, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof(optval)) == -1) {
        return -1;
    }
    return optval;
}

static jint netty_epoll_native_isTcpNoDelay(JNIEnv* env, jclass clazz, jint fd) {
    int optval;
    if (getOption(env, fd, IPPROTO_TCP, TCP_NODELAY, &optval, sizeof(optval)) == -1) {
        return -1;
    }
    return optval;
}

static jint netty_epoll_native_getReceiveBufferSize(JNIEnv* env, jclass clazz, jint fd) {
    int optval;
    if (getOption(env, fd, SOL_SOCKET, SO_RCVBUF, &optval, sizeof(optval)) == -1) {
        return -1;
    }
    return optval;
}

static jint netty_epoll_native_getSendBufferSize(JNIEnv* env, jclass clazz, jint fd) {
    int optval;
    if (getOption(env, fd, SOL_SOCKET, SO_SNDBUF, &optval, sizeof(optval)) == -1) {
        return -1;
    }
    return optval;
}

static jint netty_epoll_native_isTcpCork(JNIEnv* env, jclass clazz, jint fd) {
    int optval;
    if (getOption(env, fd, SOL_TCP, TCP_CORK, &optval, sizeof(optval)) == -1) {
        return -1;
    }
    return optval;
}

static jint netty_epoll_native_getSoLinger(JNIEnv* env, jclass clazz, jint fd) {
    struct linger optval;
    if (getOption(env, fd, SOL_SOCKET, SO_LINGER, &optval, sizeof(optval)) == -1) {
        return -1;
    }
    if (optval.l_onoff == 0) {
        return -1;
    } else {
        return optval.l_linger;
    }
}

static jint netty_epoll_native_getSoError(JNIEnv* env, jclass clazz, jint fd) {
    int optval = 0;
    if (getOption(env, fd, SOL_SOCKET, SO_ERROR, &optval, sizeof(optval)) == -1) {
        return optval;
    }
    return 0;
}

static jint netty_epoll_native_getTrafficClass(JNIEnv* env, jclass clazz, jint fd) {
    int optval;
    if (getOption(env, fd, IPPROTO_IP, IP_TOS, &optval, sizeof(optval)) == -1) {
        return -1;
    }
    return optval;
}

static jint netty_epoll_native_isBroadcast(JNIEnv* env, jclass clazz, jint fd) {
    int optval;
    if (getOption(env, fd, SOL_SOCKET, SO_BROADCAST, &optval, sizeof(optval)) == -1) {
        return -1;
    }
    return optval;
}

static jint netty_epoll_native_getTcpKeepIdle(JNIEnv* env, jclass clazz, jint fd) {
    int optval;
    if (getOption(env, fd, SOL_TCP, TCP_KEEPIDLE, &optval, sizeof(optval)) == -1) {
        return -1;
    }
    return optval;
}

static jint netty_epoll_native_getTcpKeepIntvl(JNIEnv* env, jclass clazz, jint fd) {
    int optval;
    if (getOption(env, fd, SOL_TCP, TCP_KEEPINTVL, &optval, sizeof(optval)) == -1) {
        return -1;
    }
    return optval;
}

static jint netty_epoll_native_getTcpKeepCnt(JNIEnv* env, jclass clazz, jint fd) {
     int optval;
     if (getOption(env, fd, SOL_TCP, TCP_KEEPCNT, &optval, sizeof(optval)) == -1) {
         return -1;
     }
     return optval;
}

static void netty_epoll_native_tcpInfo0(JNIEnv* env, jclass clazz, jint fd, jintArray array) {
     struct tcp_info tcp_info;
     if (getOption(env, fd, SOL_TCP, TCP_INFO, &tcp_info, sizeof(tcp_info)) == -1) {
         return;
     }
     unsigned int cArray[32];
     cArray[0] = tcp_info.tcpi_state;
     cArray[1] = tcp_info.tcpi_ca_state;
     cArray[2] = tcp_info.tcpi_retransmits;
     cArray[3] = tcp_info.tcpi_probes;
     cArray[4] = tcp_info.tcpi_backoff;
     cArray[5] = tcp_info.tcpi_options;
     cArray[6] = tcp_info.tcpi_snd_wscale;
     cArray[7] = tcp_info.tcpi_rcv_wscale;
     cArray[8] = tcp_info.tcpi_rto;
     cArray[9] = tcp_info.tcpi_ato;
     cArray[10] = tcp_info.tcpi_snd_mss;
     cArray[11] = tcp_info.tcpi_rcv_mss;
     cArray[12] = tcp_info.tcpi_unacked;
     cArray[13] = tcp_info.tcpi_sacked;
     cArray[14] = tcp_info.tcpi_lost;
     cArray[15] = tcp_info.tcpi_retrans;
     cArray[16] = tcp_info.tcpi_fackets;
     cArray[17] = tcp_info.tcpi_last_data_sent;
     cArray[18] = tcp_info.tcpi_last_ack_sent;
     cArray[19] = tcp_info.tcpi_last_data_recv;
     cArray[20] = tcp_info.tcpi_last_ack_recv;
     cArray[21] = tcp_info.tcpi_pmtu;
     cArray[22] = tcp_info.tcpi_rcv_ssthresh;
     cArray[23] = tcp_info.tcpi_rtt;
     cArray[24] = tcp_info.tcpi_rttvar;
     cArray[25] = tcp_info.tcpi_snd_ssthresh;
     cArray[26] = tcp_info.tcpi_snd_cwnd;
     cArray[27] = tcp_info.tcpi_advmss;
     cArray[28] = tcp_info.tcpi_reordering;
     cArray[29] = tcp_info.tcpi_rcv_rtt;
     cArray[30] = tcp_info.tcpi_rcv_space;
     cArray[31] = tcp_info.tcpi_total_retrans;

     (*env)->SetIntArrayRegion(env, array, 0, 32, cArray);
}

static jstring netty_epoll_native_kernelVersion(JNIEnv* env, jclass clazz) {
    struct utsname name;

    int res = uname(&name);
    if (res == 0) {
        return (*env)->NewStringUTF(env, name.release);
    }
    int err = errno;
    throwRuntimeException(env, exceptionMessage("uname() failed: ", err));
    return NULL;
}

static jint netty_epoll_native_iovMax(JNIEnv* env, jclass clazz) {
    return IOV_MAX;
}

static jint netty_epoll_native_uioMaxIov(JNIEnv* env, jclass clazz) {
    return UIO_MAXIOV;
}

static jboolean netty_epoll_native_isSupportingSendmmsg(JNIEnv* env, jclass clazz) {
    if (sendmmsg) {
        return JNI_TRUE;
    }
    return JNI_FALSE;
}

static jint netty_epoll_native_errnoENOENT(JNIEnv* env, jclass clazz) {
    return ENOENT;
}

static jint netty_epoll_native_errnoENOTCONN(JNIEnv* env, jclass clazz) {
    return ENOTCONN;
}

static jint netty_epoll_native_errnoEBADF(JNIEnv* env, jclass clazz) {
    return EBADF;
}

static jint netty_epoll_native_errnoEPIPE(JNIEnv* env, jclass clazz) {
    return EPIPE;
}

static jint netty_epoll_native_errnoECONNRESET(JNIEnv* env, jclass clazz) {
    return ECONNRESET;
}

static jint netty_epoll_native_errnoEAGAIN(JNIEnv* env, jclass clazz) {
    return EAGAIN;
}

static jint netty_epoll_native_errnoEWOULDBLOCK(JNIEnv* env, jclass clazz) {
    return EWOULDBLOCK;
}

static jint netty_epoll_native_errnoEINPROGRESS(JNIEnv* env, jclass clazz) {
    return EINPROGRESS;
}

static jint netty_epoll_native_errorECONNREFUSED(JNIEnv* env, jclass clazz) {
    return ECONNREFUSED;
}

static jint netty_epoll_native_errorEISCONN(JNIEnv* env, jclass clazz) {
    return EISCONN;
}

static jint netty_epoll_native_errorEALREADY(JNIEnv* env, jclass clazz) {
    return EALREADY;
}

static jint netty_epoll_native_errorENETUNREACH(JNIEnv* env, jclass clazz) {
    return ENETUNREACH;
}

static jstring netty_epoll_native_strError(JNIEnv* env, jclass clazz, jint error) {
    return (*env)->NewStringUTF(env, strerror(error));
}

static jint netty_epoll_native_socketDomain(JNIEnv* env, jclass clazz) {
    int fd = socket(PF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd == -1) {
        return -errno;
    }
    return fd;
}

static jint netty_epoll_native_bindDomainSocket(JNIEnv* env, jclass clazz, jint fd, jstring socketPath) {
    struct sockaddr_un addr;

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;

    const char* socket_path = (*env)->GetStringUTFChars(env, socketPath, 0);
    memcpy(addr.sun_path, socket_path, strlen(socket_path));

    if (unlink(socket_path) == -1 && errno != ENOENT) {
        return -errno;
    }

    int res = bind(fd, (struct sockaddr*) &addr, sizeof(addr));
    (*env)->ReleaseStringUTFChars(env, socketPath, socket_path);

    if (res == -1) {
        return -errno;
    }
    return res;
}

static jint netty_epoll_native_connectDomainSocket(JNIEnv* env, jclass clazz, jint fd, jstring socketPath) {
    struct sockaddr_un addr;

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;

    const char* socket_path = (*env)->GetStringUTFChars(env, socketPath, 0);
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    int res;
    int err;
    do {
        res = connect(fd, (struct sockaddr*) &addr, sizeof(addr));
    } while (res == -1 && ((err = errno) == EINTR));

    (*env)->ReleaseStringUTFChars(env, socketPath, socket_path);

    if (res < 0) {
        return -err;
    }
    return 0;
}

static jint netty_epoll_native_recvFd0(JNIEnv* env, jclass clazz, jint fd) {
    int socketFd;
    struct msghdr descriptorMessage = { 0 };
    struct iovec iov[1] = { 0 };
    char control[CMSG_SPACE(sizeof(int))] = { 0 };
    char iovecData[1];

    descriptorMessage.msg_control = control;
    descriptorMessage.msg_controllen = sizeof(control);
    descriptorMessage.msg_iov = iov;
    descriptorMessage.msg_iovlen = 1;
    iov[0].iov_base = iovecData;
    iov[0].iov_len = sizeof(iovecData);

    ssize_t res;
    int err;

    for (;;) {
        do {
            res = recvmsg(fd, &descriptorMessage, 0);
            // Keep on reading if we was interrupted
        } while (res == -1 && ((err = errno) == EINTR));

        if (res == 0) {
            return 0;
        }

        if (res < 0) {
            return -err;
        }

        struct cmsghdr* cmsg = CMSG_FIRSTHDR(&descriptorMessage);
        if (!cmsg) {
            return -errno;
        }

        if ((cmsg->cmsg_len == CMSG_LEN(sizeof(int))) && (cmsg->cmsg_level == SOL_SOCKET) && (cmsg->cmsg_type == SCM_RIGHTS)) {
            socketFd = *((int *) CMSG_DATA(cmsg));
            // set as non blocking as we want to use it with epoll
            if (fcntl(socketFd, F_SETFL, O_NONBLOCK) == -1) {
                err = errno;
                close(socketFd);
                return -err;
            }
            return socketFd;
        }
    }
}

static jint netty_epoll_native_sendFd0(JNIEnv* env, jclass clazz, jint socketFd, jint fd) {
    struct msghdr descriptorMessage = { 0 };
    struct iovec iov[1] = { 0 };
    char control[CMSG_SPACE(sizeof(int))] = { 0 };
    char iovecData[1];

    descriptorMessage.msg_control = control;
    descriptorMessage.msg_controllen = sizeof(control);
    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&descriptorMessage);

    if (cmsg) {
        cmsg->cmsg_len = CMSG_LEN(sizeof(int));
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        *((int *)CMSG_DATA(cmsg)) = fd;
        descriptorMessage.msg_iov = iov;
        descriptorMessage.msg_iovlen = 1;
        iov[0].iov_base = iovecData;
        iov[0].iov_len = sizeof(iovecData);

        size_t res;
        int err;
        do {
            res = sendmsg(socketFd, &descriptorMessage, 0);
        // keep on writing if it was interrupted
        } while (res == -1 && ((err = errno) == EINTR));

        if (res < 0) {
            return -err;
        }
        return (jint) res;
    }
    return -1;
}

static jint netty_epoll_native_epollet(JNIEnv* env, jclass clazz) {
    return EPOLLET;
}

static jint netty_epoll_native_epollerr(JNIEnv* env, jclass clazz) {
    return EPOLLERR;
}

static jint netty_epoll_native_epollin(JNIEnv* env, jclass clazz) {
    return EPOLLIN;
}

static jint netty_epoll_native_epollout(JNIEnv* env, jclass clazz) {
    return EPOLLOUT;
}

static jint netty_epoll_native_epollrdhup(JNIEnv* env, jclass clazz) {
    return EPOLLRDHUP;
}

static jint netty_epoll_native_sizeofEpollEvent(JNIEnv* env, jclass clazz) {
    return sizeof(struct epoll_event);
}

static jint netty_epoll_native_offsetofEpollData(JNIEnv* env, jclass clazz) {
    return offsetof(struct epoll_event, data);
}

static jint netty_epoll_native_sizeOfjlong(JNIEnv* env, jclass clazz) {
    return sizeof(jlong);
}

static jlong netty_epoll_native_ssizeMax(JNIEnv* env, jclass clazz) {
    return SSIZE_MAX;
}

static jboolean netty_epoll_native_isSupportingTcpFastopen(JNIEnv* env, jclass clazz) {
    int fastopen = 0;
    getSysctlValue("/proc/sys/net/ipv4/tcp_fastopen", &fastopen);
    if (fastopen > 0) {
        return JNI_TRUE;
    }
    return JNI_FALSE;
}

static jint netty_epoll_native_open0(JNIEnv* env, jclass clazz, jstring path) {
    const char* f_path = (*env)->GetStringUTFChars(env, path, 0);

    int res = open(f_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    (*env)->ReleaseStringUTFChars(env, path, f_path);

    if (res < 0) {
        return -errno;
    }
    return res;
}

// Loading and Unloading

static char* netty_unix_util_prepend(const char* prefix, const char* str) {
    if (prefix == NULL) {
        char* result = (char*) malloc(sizeof(char) * (strlen(str) + 1));
        strcpy(result, str);
        return result;
    }
    char* result = (char*) malloc(sizeof(char) * (strlen(prefix) + strlen(str) + 1));
    strcpy(result, prefix);
    strcat(result, str);
    return result;
}

static char* netty_unix_util_rstrstr(char* s1rbegin, const char* s1rend, const char* s2) {
    size_t s2len = strlen(s2);
    char *s = s1rbegin - s2len;

    for (; s >= s1rend; --s) {
        if (strncmp(s, s2, s2len) == 0) {
            return s;
        }
    }
    return NULL;
}

static char* netty_unix_util_strstr_last(const char* haystack, const char* needle) {
    char* prevptr = NULL;
    char* ptr = (char*) haystack;

    while ((ptr = strstr(ptr, needle)) != NULL) {
        // Just store the ptr and continue searching.
        prevptr = ptr;
        ++ptr;
    }
    return prevptr;
}

char* netty_unix_util_parse_package_prefix(const char* libraryPathName, const char* libraryName, jint* status) {
    char* packageNameEnd = netty_unix_util_strstr_last(libraryPathName, libraryName);
    if (packageNameEnd == NULL) {
        *status = JNI_ERR;
        return NULL;
    }
    char* packagePrefix = netty_unix_util_rstrstr(packageNameEnd, libraryPathName, "lib");
    if (packagePrefix == NULL) {
        *status = JNI_ERR;
        return NULL;
    }
    packagePrefix += 3;
    if (packagePrefix == packageNameEnd) {
        return NULL;
    }
    // packagePrefix length is > 0
    // Make a copy so we can modify the value without impacting libraryPathName.
    size_t packagePrefixLen = packageNameEnd - packagePrefix;
    packagePrefix = strndup(packagePrefix, packagePrefixLen);
    // Make sure the packagePrefix is in the correct format for the JNI functions it will be used with.
    char* temp = packagePrefix;
    packageNameEnd = packagePrefix + packagePrefixLen;
    // Package names must be sanitized, in JNI packages names are separated by '/' characters.
    for (; temp != packageNameEnd; ++temp) {
        if (*temp == '_') {
            *temp = '/';
        }
    }
    // Make sure packagePrefix is terminated with the '/' JNI package separator.
    if(*(--temp) != '/') {
        temp = packagePrefix;
        packagePrefix = netty_unix_util_prepend(packagePrefix, "/");
        free(temp);
    }
    return packagePrefix;
}

// JNI Method Registration Table Begin
static const JNINativeMethod statically_referenced_fixed_method_table[] = {
    { "epollet", "()I", (void *) netty_epoll_native_epollet },
    { "epollin", "()I", (void *) netty_epoll_native_epollin },
    { "epollout", "()I", (void *) netty_epoll_native_epollout },
    { "epollrdhup", "()I", (void *) netty_epoll_native_epollrdhup },
    { "epollerr", "()I", (void *) netty_epoll_native_epollerr },
    { "isSupportingSendmmsg", "()Z", (void *) netty_epoll_native_isSupportingSendmmsg },
    { "isSupportingTcpFastopen", "()Z", (void *) netty_epoll_native_isSupportingTcpFastopen },
    { "kernelVersion", "()Ljava/lang/String;", (void *) netty_epoll_native_kernelVersion },
    { "iovMax", "()I", (void *) netty_epoll_native_iovMax },
    { "uioMaxIov", "()I", (void *) netty_epoll_native_uioMaxIov },
    { "ssizeMax", "()J", (void *) netty_epoll_native_ssizeMax },
    { "errnoENOENT", "()I", (void *) netty_epoll_native_errnoENOENT },
    { "errnoENOTCONN", "()I", (void *) netty_epoll_native_errnoENOTCONN },
    { "errnoEBADF", "()I", (void *) netty_epoll_native_errnoEBADF },
    { "errnoEPIPE", "()I", (void *) netty_epoll_native_errnoEPIPE },
    { "errnoECONNRESET", "()I", (void *) netty_epoll_native_errnoECONNRESET },
    { "errnoEAGAIN", "()I", (void *) netty_epoll_native_errnoEAGAIN },
    { "errnoEWOULDBLOCK", "()I", (void *) netty_epoll_native_errnoEWOULDBLOCK },
    { "errnoEINPROGRESS", "()I", (void *) netty_epoll_native_errnoEINPROGRESS },
    { "errorECONNREFUSED", "()I", (void *) netty_epoll_native_errorECONNREFUSED },
    { "errorEISCONN", "()I", (void *) netty_epoll_native_errorEISCONN },
    { "errorEALREADY", "()I", (void *) netty_epoll_native_errorEALREADY },
    { "errorENETUNREACH", "()I", (void *) netty_epoll_native_errorENETUNREACH },
    { "strError", "(I)Ljava/lang/String;", (void *) netty_epoll_native_strError }
};
static const jint statically_referenced_fixed_method_table_size = sizeof(statically_referenced_fixed_method_table) / sizeof(statically_referenced_fixed_method_table[0]);
static const JNINativeMethod fixed_method_table[] = {
    { "eventFd", "()I", (void *) netty_epoll_native_eventFd },
    { "eventFdWrite", "(IJ)V", (void *) netty_epoll_native_eventFdWrite },
    { "eventFdRead", "(I)V", (void *) netty_epoll_native_eventFdRead },
    { "epollCreate", "()I", (void *) netty_epoll_native_epollCreate },
    { "epollWait0", "(IJII)I", (void *) netty_epoll_native_epollWait0 },
    { "epollCtlAdd0", "(III)I", (void *) netty_epoll_native_epollCtlAdd0 },
    { "epollCtlMod0", "(III)I", (void *) netty_epoll_native_epollCtlMod0 },
    { "epollCtlDel0", "(II)I", (void *) netty_epoll_native_epollCtlDel0 },
    // "sendmmsg0" has a dynamic signature
    { "sizeofEpollEvent", "()I", (void *) netty_epoll_native_sizeofEpollEvent },
    { "offsetofEpollData", "()I", (void *) netty_epoll_native_offsetofEpollData },
    { "shutdown0", "(IZZ)I", (void *) netty_epoll_native_shutdown0 },
    { "bind", "(I[BII)I", (void *) netty_epoll_native_bind },
    { "listen0", "(II)I", (void *) netty_epoll_native_listen0 },
    { "connect", "(I[BII)I", (void *) netty_epoll_native_connect },
    { "finishConnect0", "(I)I", (void *) netty_epoll_native_finishConnect0 },
    { "accept0", "(I[B)I", (void *) netty_epoll_native_accept0 },
    { "remoteAddress0", "(I)[B", (void *) netty_epoll_native_remoteAddress0 },
    { "localAddress0", "(I)[B", (void *) netty_epoll_native_localAddress0 },
    { "socketDgram", "()I", (void *) netty_epoll_native_socketDgram },
    { "socketStream", "()I", (void *) netty_epoll_native_socketStream },
    { "socketDomain", "()I", (void *) netty_epoll_native_socketDomain },
    { "sendTo0", "(ILjava/nio/ByteBuffer;II[BII)I", (void *) netty_epoll_native_sendTo0 },
    { "sendToAddress0", "(IJII[BII)I", (void *) netty_epoll_native_sendToAddress0 },
    { "sendToAddresses", "(IJI[BII)I", (void *) netty_epoll_native_sendToAddresses },
    // "recvFrom" has a dynamic signature
    // "recvFromAddress" has a dynamic signature
    { "recvFd0", "(I)I", (void *) netty_epoll_native_recvFd0 },
    { "sendFd0", "(II)I", (void *) netty_epoll_native_sendFd0 },
    { "bindDomainSocket", "(ILjava/lang/String;)I", (void *) netty_epoll_native_bindDomainSocket },
    { "connectDomainSocket", "(ILjava/lang/String;)I", (void *) netty_epoll_native_connectDomainSocket },
    { "setTcpNoDelay", "(II)V", (void *) netty_epoll_native_setTcpNoDelay },
    { "setReusePort", "(II)V", (void *) netty_epoll_native_setReusePort },
    { "setBroadcast", "(II)V", (void *) netty_epoll_native_setBroadcast },
    { "setReuseAddress", "(II)V", (void *) netty_epoll_native_setReuseAddress },
    { "setReceiveBufferSize", "(II)V", (void *) netty_epoll_native_setReceiveBufferSize },
    { "setSendBufferSize", "(II)V", (void *) netty_epoll_native_setSendBufferSize },
    { "setKeepAlive", "(II)V", (void *) netty_epoll_native_setKeepAlive },
    { "setSoLinger", "(II)V", (void *) netty_epoll_native_setSoLinger },
    { "setTrafficClass", "(II)V", (void *) netty_epoll_native_setTrafficClass },
    //{ "isKeepAlive", "(I)I", (void *) netty_epoll_native_isKeepAlive },
    { "isTcpNoDelay", "(I)I", (void *) netty_epoll_native_isTcpNoDelay },
    { "isBroadcast", "(I)I", (void *) netty_epoll_native_isBroadcast },
    { "isReuseAddress", "(I)I", (void *) netty_epoll_native_isReuseAddress },
    { "isReusePort", "(I)I", (void *) netty_epoll_native_isReusePort },
    { "getReceiveBufferSize", "(I)I", (void *) netty_epoll_native_getReceiveBufferSize },
    { "getSendBufferSize", "(I)I", (void *) netty_epoll_native_getSendBufferSize },
    { "getSoLinger", "(I)I", (void *) netty_epoll_native_getSoLinger },
    { "getTrafficClass", "(I)I", (void *) netty_epoll_native_getTrafficClass },
    { "getSoError", "(I)I", (void *) netty_epoll_native_getSoError },
    { "createCrc32Socket", "()I", (void *) netty_epoll_native_create_crc32_socket },
    { "connectCrc32Socket", "(I)I", (void *) netty_epoll_native_connect_crc32_socket },
    { "close0", "(I)I", (void *) netty_epoll_native_close0 },
    { "open0", "(Ljava/lang/String;)I", (void *) netty_epoll_native_open0 },
    { "write0", "(ILjava/nio/ByteBuffer;II)I", (void *) netty_epoll_native_write0 },
    { "writeAddress0", "(IJII)I", (void *) netty_epoll_native_writeAddress0 },
    { "writevAddresses0", "(IJI)J", (void *) netty_epoll_native_writevAddresses0 },
    { "writev0", "(I[Ljava/nio/ByteBuffer;II)J", (void *) netty_epoll_native_writev0 },
    { "read0", "(ILjava/nio/ByteBuffer;II)I", (void *) netty_epoll_native_read0 },
    { "readAddress0", "(IJII)I", (void *) netty_epoll_native_readAddress0 },
    { "tcpInfo0", "(I[I)V", (void *) netty_epoll_native_tcpInfo0 },

};
static const jint fixed_method_table_size = sizeof(fixed_method_table) / sizeof(fixed_method_table[0]);

static jint dynamicMethodsTableSize() {
    return fixed_method_table_size + 5;
}

static JNINativeMethod* createDynamicMethodsTable(const char* packagePrefix) {
    JNINativeMethod* dynamicMethods = malloc(sizeof(JNINativeMethod) * dynamicMethodsTableSize());
    memcpy(dynamicMethods, fixed_method_table, sizeof(fixed_method_table));

    char* dynamicTypeName = netty_unix_util_prepend(packagePrefix, "io/netty/channel/epoll/NativeDatagramPacketArray$NativeDatagramPacket;II)I");
    JNINativeMethod* dynamicMethod = &dynamicMethods[fixed_method_table_size];
    dynamicMethod->name = "sendmmsg0";
    dynamicMethod->signature = netty_unix_util_prepend("(I[L", dynamicTypeName);
    dynamicMethod->fnPtr = (void *) netty_epoll_native_sendmmsg0;

    ++dynamicMethod;
    dynamicTypeName = netty_unix_util_prepend(packagePrefix, "io/netty/channel/epoll/DatagramSocketAddress;");
    dynamicMethod->name = "recvFrom";
    dynamicMethod->signature = netty_unix_util_prepend("(ILjava/nio/ByteBuffer;II)L", dynamicTypeName);
    dynamicMethod->fnPtr = (void *) netty_epoll_native_recvFrom;
    free(dynamicTypeName);

    ++dynamicMethod;
    dynamicTypeName = netty_unix_util_prepend(packagePrefix, "io/netty/channel/epoll/DatagramSocketAddress;");
    dynamicMethod->name = "recvFromAddress";
    dynamicMethod->signature = netty_unix_util_prepend("(IJII)L", dynamicTypeName);
    dynamicMethod->fnPtr = (void *) netty_epoll_native_recvFromAddress;
    free(dynamicTypeName);

    ++dynamicMethod;
    dynamicTypeName = netty_unix_util_prepend(packagePrefix, "io/netty/channel/DefaultFileRegion;JJJ)J");
    dynamicMethod->name = "sendFile";
    dynamicMethod->signature = netty_unix_util_prepend("(IL", dynamicTypeName);
    dynamicMethod->fnPtr = (void *) netty_epoll_native_sendfile0;
    free(dynamicTypeName);

    ++dynamicMethod;
    dynamicTypeName = netty_unix_util_prepend(packagePrefix, "io/netty/channel/CRC32FileRegion;JJJI)J");
    dynamicMethod->name = "sendFileCrc32";
    dynamicMethod->signature = netty_unix_util_prepend("(IL", dynamicTypeName);
    dynamicMethod->fnPtr = (void *) netty_epoll_native_sendfile_crc32;
    free(dynamicTypeName);

    return dynamicMethods;
}

static void freeDynamicMethodsTable(JNINativeMethod* dynamicMethods) {
    jint fullMethodTableSize = dynamicMethodsTableSize();
    jint i = fixed_method_table_size;
    for (; i < fullMethodTableSize; ++i) {
        free(dynamicMethods[i].signature);
    }
    free(dynamicMethods);
}
// JNI Method Registration Table End

jint netty_unix_util_register_natives(JNIEnv* env, const char* packagePrefix, const char* className, const JNINativeMethod* methods, jint numMethods) {
    char* nettyClassName = netty_unix_util_prepend(packagePrefix, className);
    jclass nativeCls = (*env)->FindClass(env, nettyClassName);
    free(nettyClassName);
    nettyClassName = NULL;
    if (nativeCls == NULL) {
        return JNI_ERR;
    }
    return (*env)->RegisterNatives(env, nativeCls, methods, numMethods);
}


#define FIND_CLASS_HELPER(clazzNameVar, clazzName) \
    char* clazzNameVar##_clzName = netty_unix_util_prepend(packagePrefix, clazzName); \
    jclass clazzNameVar = (*env)->FindClass(env, clazzNameVar##_clzName); \
    free(clazzNameVar##_clzName);

jint netty_epoll_native_JNI_OnLoad(JNIEnv* env, const char* packagePrefix) {

    // We must register the statically referenced methods first!
    if (netty_unix_util_register_natives(env,
            packagePrefix,
            "io/netty/channel/epoll/NativeStaticallyReferencedJniMethods",
            statically_referenced_fixed_method_table,
            statically_referenced_fixed_method_table_size) != 0) {
        return JNI_ERR;
    }
    // Register the methods which are not referenced by static member variables
    JNINativeMethod* dynamicMethods = createDynamicMethodsTable(packagePrefix);
    if (netty_unix_util_register_natives(env,
            packagePrefix,
            "io/netty/channel/epoll/Native",
            dynamicMethods,
            dynamicMethodsTableSize()) != 0) {
        freeDynamicMethodsTable(dynamicMethods);
        return JNI_ERR;
    }
    freeDynamicMethodsTable(dynamicMethods);
    dynamicMethods = NULL;

    jclass localRuntimeExceptionClass = (*env)->FindClass(env, "java/lang/RuntimeException");
    if (localRuntimeExceptionClass == NULL) {
        // pending exception...
        return JNI_ERR;
    }
    runtimeExceptionClass = (jclass) (*env)->NewGlobalRef(env, localRuntimeExceptionClass);
    if (runtimeExceptionClass == NULL) {
        // out-of-memory!
        throwOutOfMemoryError(env);
        return JNI_ERR;
    }

    FIND_CLASS_HELPER(localNetUtilClass, "io/netty/util/NetUtil")
    if (localNetUtilClass == NULL) {
        // pending exception...
        return JNI_ERR;
    }
    netUtilClass = (jclass) (*env)->NewGlobalRef(env, localNetUtilClass);
    if (netUtilClass == NULL) {
        // out-of-memory!
        throwOutOfMemoryError(env);
        return JNI_ERR;
    }

    netUtilClassIpv4PreferredMethodId = (*env)->GetStaticMethodID(env, netUtilClass, "isIpV4StackPreferred", "()Z" );
    if (netUtilClassIpv4PreferredMethodId == NULL) {
        // position method was not found.. something is wrong so bail out
        throwRuntimeException(env, "failed to get method ID: NetUild.isIpV4StackPreferred()");
        return JNI_ERR;
    }
    // cache classes that are used within other jni methods for performance reasons
    jclass localClosedChannelExceptionClass = (*env)->FindClass(env, "java/nio/channels/ClosedChannelException");
    if (localClosedChannelExceptionClass == NULL) {
        // pending exception...
        return JNI_ERR;
    }
    closedChannelExceptionClass = (jclass) (*env)->NewGlobalRef(env, localClosedChannelExceptionClass);
    if (closedChannelExceptionClass == NULL) {
        // out-of-memory!
        throwOutOfMemoryError(env);
        return JNI_ERR;
    }
    closedChannelExceptionMethodId = (*env)->GetMethodID(env, closedChannelExceptionClass, "<init>", "()V");
    if (closedChannelExceptionMethodId == NULL) {
        throwRuntimeException(env, "failed to get method ID: ClosedChannelException.<init>()");
        return JNI_ERR;
    }

    jclass localIoExceptionClass = (*env)->FindClass(env, "java/io/IOException");
    if (localIoExceptionClass == NULL) {
        // pending exception...
        return JNI_ERR;
    }
    ioExceptionClass = (jclass) (*env)->NewGlobalRef(env, localIoExceptionClass);
    if (ioExceptionClass == NULL) {
        // out-of-memory!
        throwOutOfMemoryError(env);
        return JNI_ERR;
    }

    jclass localInetSocketAddressClass = (*env)->FindClass(env, "java/net/InetSocketAddress");
    if (localIoExceptionClass == NULL) {
        // pending exception...
        return JNI_ERR;
    }
    inetSocketAddressClass = (jclass) (*env)->NewGlobalRef(env, localInetSocketAddressClass);
    if (inetSocketAddressClass == NULL) {
        // out-of-memory!
        throwOutOfMemoryError(env);
        return JNI_ERR;
    }

    FIND_CLASS_HELPER(localDatagramSocketAddressClass, "io/netty/channel/epoll/DatagramSocketAddress")
    if (localDatagramSocketAddressClass == NULL) {
        // pending exception...
        return JNI_ERR;
    }
    datagramSocketAddressClass = (jclass) (*env)->NewGlobalRef(env, localDatagramSocketAddressClass);
    if (datagramSocketAddressClass == NULL) {
        // out-of-memory!
        throwOutOfMemoryError(env);
        return JNI_ERR;
    }

    void* mem = malloc(1);
    if (mem == NULL) {
        throwOutOfMemoryError(env);
        return JNI_ERR;
    }
    jobject directBuffer = (*env)->NewDirectByteBuffer(env, mem, 1);
    if (directBuffer == NULL) {
        free(mem);

        throwOutOfMemoryError(env);
        return JNI_ERR;
    }

    jclass cls = (*env)->GetObjectClass(env, directBuffer);

    // Get the method id for Buffer.position() and Buffer.limit(). These are used as fallback if
    // it is not possible to obtain the position and limit using the fields directly.
    posId = (*env)->GetMethodID(env, cls, "position", "()I");
    if (posId == NULL) {
        free(mem);

        // position method was not found.. something is wrong so bail out
        throwRuntimeException(env, "failed to get method ID: ByteBuffer.position()");
        return JNI_ERR;
    }

    limitId = (*env)->GetMethodID(env, cls, "limit", "()I");
    if (limitId == NULL) {
        free(mem);

        // limit method was not found.. something is wrong so bail out
        throwRuntimeException(env, "failed to get method ID: ByteBuffer.limit()");
        return JNI_ERR;
    }
    updatePosId = (*env)->GetMethodID(env, cls, "position", "(I)Ljava/nio/Buffer;");
    if (updatePosId == NULL) {
        free(mem);

        // position method was not found.. something is wrong so bail out
        throwRuntimeException(env, "failed to fet method ID: ByteBuffer.position(int)");
        return JNI_ERR;
    }
    // Try to get the ids of the position and limit fields. We later then check if we was able
    // to find them and if so use them get the position and limit of the buffer. This is
    // much faster then call back into java via (*env)->CallIntMethod(...).
    posFieldId = (*env)->GetFieldID(env, cls, "position", "I");
    if (posFieldId == NULL) {
        // this is ok as we can still use the method so just clear the exception
        (*env)->ExceptionClear(env);
    }
    limitFieldId = (*env)->GetFieldID(env, cls, "limit", "I");
    if (limitFieldId == NULL) {
        // this is ok as we can still use the method so just clear the exception
        (*env)->ExceptionClear(env);
    }

    free(mem);

    FIND_CLASS_HELPER(fileRegionCls, "io/netty/channel/DefaultFileRegion")
    if (fileRegionCls == NULL) {
        // pending exception...
        return JNI_ERR;
    }
    FIND_CLASS_HELPER(crc32FileRegionCls, "io/netty/channel/CRC32FileRegion")
    if (crc32FileRegionCls == NULL) {
        // pending exception...
        return JNI_ERR;
    }
    fileChannelFieldId = (*env)->GetFieldID(env, fileRegionCls, "file", "Ljava/nio/channels/FileChannel;");
    if (fileChannelFieldId == NULL) {
        throwRuntimeException(env, "failed to get field ID: DefaultFileRegion.file");
        return JNI_ERR;
    }
    transferedFieldId = (*env)->GetFieldID(env, fileRegionCls, "transfered", "J");
    if (transferedFieldId == NULL) {
        throwRuntimeException(env, "failed to get field ID: DefaultFileRegion.transfered");
        return JNI_ERR;
    }
    crc32FileChannelFieldId = (*env)->GetFieldID(env, crc32FileRegionCls, "channel", "Ljava/nio/channels/FileChannel;");
    if (fileChannelFieldId == NULL) {
        throwRuntimeException(env, "failed to get field ID: CRC32FileRegion.channel");
        return JNI_ERR;
    }
    crc32TransferedFieldId = (*env)->GetFieldID(env, crc32FileRegionCls, "transfered", "J");
    if (transferedFieldId == NULL) {
        throwRuntimeException(env, "failed to get field ID: CRC32FileRegion.transfered");
        return JNI_ERR;
    }
    jclass fileChannelCls = (*env)->FindClass(env, "sun/nio/ch/FileChannelImpl");
    if (fileChannelCls == NULL) {
        // pending exception...
        return JNI_ERR;
    }
    fileDescriptorFieldId = (*env)->GetFieldID(env, fileChannelCls, "fd", "Ljava/io/FileDescriptor;");
    if (fileDescriptorFieldId == NULL) {
        throwRuntimeException(env, "failed to get field ID: FileChannelImpl.fd");
        return JNI_ERR;
    }

    jclass fileDescriptorCls = (*env)->FindClass(env, "java/io/FileDescriptor");
    if (fileDescriptorCls == NULL) {
        // pending exception...
        return JNI_ERR;
    }
    fdFieldId = (*env)->GetFieldID(env, fileDescriptorCls, "fd", "I");
    if (fdFieldId == NULL) {
        throwRuntimeException(env, "failed to get field ID: FileDescriptor.fd");
        return JNI_ERR;
    }

    inetSocketAddrMethodId = (*env)->GetMethodID(env, inetSocketAddressClass, "<init>", "(Ljava/lang/String;I)V");
    if (inetSocketAddrMethodId == NULL) {
        throwRuntimeException(env, "failed to get method ID: InetSocketAddress.<init>(String, int)");
        return JNI_ERR;
    }
    socketType = socket_type(env);

    datagramSocketAddrMethodId = (*env)->GetMethodID(env, datagramSocketAddressClass, "<init>", "(Ljava/lang/String;II)V");
    if (datagramSocketAddrMethodId == NULL) {
        throwRuntimeException(env, "failed to get method ID: DatagramSocketAddress.<init>(String, int, int)");
        return JNI_ERR;
    }

    FIND_CLASS_HELPER(nativeDatagramPacketCls, "io/netty/channel/epoll/NativeDatagramPacketArray$NativeDatagramPacket");
    if (nativeDatagramPacketCls == NULL) {
        // pending exception...
        return JNI_ERR;
    }

    packetAddrFieldId = (*env)->GetFieldID(env, nativeDatagramPacketCls, "addr", "[B");
    if (packetAddrFieldId == NULL) {
        throwRuntimeException(env, "failed to get field ID: NativeDatagramPacket.addr");
        return JNI_ERR;
    }
    packetScopeIdFieldId = (*env)->GetFieldID(env, nativeDatagramPacketCls, "scopeId", "I");
    if (packetScopeIdFieldId == NULL) {
        throwRuntimeException(env, "failed to get field ID: NativeDatagramPacket.scopeId");
        return JNI_ERR;
    }
    packetPortFieldId = (*env)->GetFieldID(env, nativeDatagramPacketCls, "port", "I");
    if (packetPortFieldId == NULL) {
        throwRuntimeException(env, "failed to get field ID: NativeDatagramPacket.port");
        return JNI_ERR;
    }
    packetMemoryAddressFieldId = (*env)->GetFieldID(env, nativeDatagramPacketCls, "memoryAddress", "J");
    if (packetMemoryAddressFieldId == NULL) {
        throwRuntimeException(env, "failed to get field ID: NativeDatagramPacket.memoryAddress");
        return JNI_ERR;
    }

    packetCountFieldId = (*env)->GetFieldID(env, nativeDatagramPacketCls, "count", "I");
    if (packetCountFieldId == NULL) {
        throwRuntimeException(env, "failed to get field ID: NativeDatagramPacket.count");
        return JNI_ERR;
    }

    if (netty_unix_filedescriptor_JNI_OnLoad(env, packagePrefix) == JNI_ERR) {
        return JNI_ERR;
    }

    return JNI_VERSION_1_6;
}

static void netty_epoll_native_JNI_OnUnLoad(JNIEnv* env) {
    if (runtimeExceptionClass != NULL) {
        (*env)->DeleteGlobalRef(env, runtimeExceptionClass);
    }
    if (ioExceptionClass != NULL) {
        (*env)->DeleteGlobalRef(env, ioExceptionClass);
    }
    if (closedChannelExceptionClass != NULL) {
        (*env)->DeleteGlobalRef(env, closedChannelExceptionClass);
    }
    if (inetSocketAddressClass != NULL) {
        (*env)->DeleteGlobalRef(env, inetSocketAddressClass);
    }
    if (datagramSocketAddressClass != NULL) {
        (*env)->DeleteGlobalRef(env, datagramSocketAddressClass);
    }
    if (netUtilClass != NULL) {
        (*env)->DeleteGlobalRef(env, netUtilClass);
    }
}

jint JNI_OnLoad(JavaVM* vm, void* reserved) {
    JNIEnv* env;
    if ((*vm)->GetEnv(vm, (void**) &env, JNI_VERSION_1_6) != JNI_OK) {
        return JNI_ERR;
    } else {
        char* packagePrefix = NULL;
        Dl_info dlinfo;
        jint status = 0;
        // We need to use an address of a function that is uniquely part of this library, so choose a static
        // function. See https://github.com/netty/netty/issues/4840.
        if (!dladdr((void*) netty_epoll_native_JNI_OnUnLoad, &dlinfo)) {
            fprintf(stderr, "FATAL: transport-native-epoll JNI call to dladdr failed!\n");
            return JNI_ERR;
        }
        packagePrefix = netty_unix_util_parse_package_prefix(dlinfo.dli_fname, "netty_transport_native_epoll", &status);
        if (status == JNI_ERR) {
            fprintf(stderr, "FATAL: transport-native-epoll JNI encountered unexpected dlinfo.dli_fname: %s\n", dlinfo.dli_fname);
            return JNI_ERR;
        }

        jint ret = netty_epoll_native_JNI_OnLoad(env, packagePrefix);

        if (packagePrefix != NULL) {
            free(packagePrefix);
            packagePrefix = NULL;
        }

        return ret;
    }
}

JNIEXPORT void JNI_OnUnload(JavaVM* vm, void* reserved) {
    JNIEnv* env;
    if ((*vm)->GetEnv(vm, (void**) &env, JNI_VERSION_1_6) != JNI_OK) {
        // Something is wrong but nothing we can do about this :(
        return;
    } else {
        // delete global references so the GC can collect them
        netty_epoll_native_JNI_OnUnLoad(env);
    }
}
