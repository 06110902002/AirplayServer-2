//
// Created by Administrator on 2019/1/29/029.
//

#include "raop_rtp_mirror.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <tgmath.h>

#include "raop.h"
#include "netutils.h"
#include "compat.h"
#include "logger.h"
#include "byteutils.h"
#include "mirror_buffer.h"
#include "stream.h"
#include "../log.h"


struct h264codec_s {
    unsigned char compatibility;
    short lengthofPPS;
    short lengthofSPS;
    unsigned char level;
    unsigned char numberOfPPS;
    unsigned char* picture_parameter_set;
    unsigned char profile_high;
    unsigned char reserved3andSPS;
    unsigned char reserved6andNAL;
    unsigned char* sequence;
    unsigned char version;
};

struct raop_rtp_mirror_s {
    logger_t *logger;
    raop_callbacks_t callbacks;

    /* Buffer to handle all resends */
    mirror_buffer_t *buffer;

    raop_rtp_mirror_t *mirror;
    /* Remote address as sockaddr */
    struct sockaddr_storage remote_saddr;
    socklen_t remote_saddr_len;

    /* MUTEX LOCKED VARIABLES START */
    /* These variables only edited mutex locked */
    int running;
    int joined;

    int flush;
    thread_handle_t thread_mirror;
    thread_handle_t thread_time;
    mutex_handle_t run_mutex;

    mutex_handle_t time_mutex;
    cond_handle_t time_cond;
    /* MUTEX LOCKED VARIABLES END */
    int mirror_data_sock, mirror_time_sock;

    unsigned short mirror_data_lport;
    unsigned short mirror_timing_rport;
    unsigned short mirror_timing_lport;
};

static int
raop_rtp_parse_remote(raop_rtp_mirror_t *raop_rtp_mirror, const unsigned char *remote, int remotelen)
{
    char current[25];
    int family;
    int ret;
    assert(raop_rtp_mirror);
    if (remotelen == 4) {
        family = AF_INET;
    } else if (remotelen == 16) {
        family = AF_INET6;
    } else {
        return -1;
    }
    memset(current, 0, sizeof(current));
    sprintf(current, "%d.%d.%d.%d", remote[0], remote[1], remote[2], remote[3]);
    logger_log(raop_rtp_mirror->logger, LOGGER_DEBUG, "raop_rtp_parse_remote ip = %s", current);
    ret = netutils_parse_address(family, current,
                                 &raop_rtp_mirror->remote_saddr,
                                 sizeof(raop_rtp_mirror->remote_saddr));
    if (ret < 0) {
        return -1;
    }
    raop_rtp_mirror->remote_saddr_len = ret;
    return 0;
}

#define NO_FLUSH (-42)
raop_rtp_mirror_t *raop_rtp_mirror_init(logger_t *logger, raop_callbacks_t *callbacks, const unsigned char *remote, int remotelen,
                                        const unsigned char *aeskey, const unsigned char *ecdh_secret, unsigned short timing_rport)
{
    raop_rtp_mirror_t *raop_rtp_mirror;

    assert(logger);
    assert(callbacks);

    raop_rtp_mirror = calloc(1, sizeof(raop_rtp_mirror_t));
    if (!raop_rtp_mirror) {
        return NULL;
    }
    raop_rtp_mirror->logger = logger;
    raop_rtp_mirror->mirror_timing_rport = timing_rport;

    memcpy(&raop_rtp_mirror->callbacks, callbacks, sizeof(raop_callbacks_t));
    raop_rtp_mirror->buffer = mirror_buffer_init(logger, aeskey, ecdh_secret);
    if (!raop_rtp_mirror->buffer) {
        free(raop_rtp_mirror);
        return NULL;
    }
    if (raop_rtp_parse_remote(raop_rtp_mirror, remote, remotelen) < 0) {
        free(raop_rtp_mirror);
        return NULL;
    }
    raop_rtp_mirror->running = 0;
    raop_rtp_mirror->joined = 1;
    raop_rtp_mirror->flush = NO_FLUSH;

    MUTEX_CREATE(raop_rtp_mirror->run_mutex);
    MUTEX_CREATE(raop_rtp_mirror->time_mutex);
    COND_CREATE(raop_rtp_mirror->time_cond);
    return raop_rtp_mirror;
}

void
raop_rtp_init_mirror_aes(raop_rtp_mirror_t *raop_rtp_mirror, uint64_t streamConnectionID)
{
    mirror_buffer_init_aes(raop_rtp_mirror->buffer, streamConnectionID);
}

/**
 * ntp
 */
static THREAD_RETVAL
raop_rtp_mirror_thread_time(void *arg)
{
    raop_rtp_mirror_t *raop_rtp_mirror = arg;
    assert(raop_rtp_mirror);
    struct sockaddr_storage saddr;
    socklen_t saddrlen;
    unsigned char packet[128];
    unsigned int packetlen;
    int first = 0;
    unsigned char time[48]={35,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
    uint64_t base = now_us();
    uint64_t rec_pts = 0;
    while (1) {
        MUTEX_LOCK(raop_rtp_mirror->run_mutex);
        if (!raop_rtp_mirror->running) {
            MUTEX_UNLOCK(raop_rtp_mirror->run_mutex);
            break;
        }
        MUTEX_UNLOCK(raop_rtp_mirror->run_mutex);
        uint64_t send_time = now_us() - base + rec_pts;

        byteutils_put_timeStamp(time, 40, send_time);
        logger_log(raop_rtp_mirror->logger, LOGGER_DEBUG, "raop_rtp_mirror_thread_time send time 48 bytes, port = %d", raop_rtp_mirror->mirror_timing_rport);
        struct sockaddr_in *addr = (struct sockaddr_in *)&raop_rtp_mirror->remote_saddr;
        addr->sin_port = htons(raop_rtp_mirror->mirror_timing_rport);
        int sendlen = sendto(raop_rtp_mirror->mirror_time_sock, (char *)time, sizeof(time), 0, (struct sockaddr *) &raop_rtp_mirror->remote_saddr, raop_rtp_mirror->remote_saddr_len);
        logger_log(raop_rtp_mirror->logger, LOGGER_DEBUG, "raop_rtp_mirror_thread_time sendlen = %d", sendlen);

        saddrlen = sizeof(saddr);
        packetlen = recvfrom(raop_rtp_mirror->mirror_time_sock, (char *)packet, sizeof(packet), 0,
                             (struct sockaddr *)&saddr, &saddrlen);
        logger_log(raop_rtp_mirror->logger, LOGGER_DEBUG, "raop_rtp_mirror_thread_time receive time packetlen = %d", packetlen);
        // 16-24 系统时钟最后一次被设定或更新的时间。
        uint64_t Reference_Timestamp = byteutils_read_timeStamp(packet, 16);
        // 24-32 NTP请求报文离开发送端时发送端的本地时间。  T1
        uint64_t Origin_Timestamp = byteutils_read_timeStamp(packet, 24);
        // 32-40 NTP请求报文到达接收端时接收端的本地时间。 T2
        uint64_t Receive_Timestamp = byteutils_read_timeStamp(packet, 32);
        // 40-48 Transmit Timestamp：应答报文离开应答者时应答者的本地时间。 T3
        uint64_t Transmit_Timestamp = byteutils_read_timeStamp(packet, 40);

        // FIXME: 先简单这样写吧
        rec_pts = Receive_Timestamp;

        if (first == 0) {
            first++;
        } else {
            struct timeval now;
            struct timespec outtime;
            MUTEX_LOCK(raop_rtp_mirror->time_mutex);
            gettimeofday(&now, NULL);
            outtime.tv_sec = now.tv_sec + 3;
            outtime.tv_nsec = now.tv_usec * 1000;
            int ret = pthread_cond_timedwait(&raop_rtp_mirror->time_cond, &raop_rtp_mirror->time_mutex, &outtime);
            MUTEX_UNLOCK(raop_rtp_mirror->time_mutex);
            //sleepms(3000);
        }
    }
    logger_log(raop_rtp_mirror->logger, LOGGER_INFO, "Exiting UDP raop_rtp_mirror_thread_time thread");
    return 0;
}
//#define DUMP_H264

#define RAOP_PACKET_LEN 32768
/**
 * 镜像
 */
static THREAD_RETVAL
raop_rtp_mirror_thread(void *arg)
{
    raop_rtp_mirror_t *raop_rtp_mirror = arg;
    int stream_fd = -1;
    unsigned char packet[128];
    memset(packet, 0 , 128);
    unsigned int readstart = 0;
    uint64_t pts_base = 0;
    uint64_t pts = 0;
    assert(raop_rtp_mirror);

#ifdef DUMP_H264
    // C 解密的
    FILE* file = fopen("/sdcard/111.h264", "wb");
    // 加密的源文件
    FILE* file_source = fopen("/sdcard/111.source", "wb");

    FILE* file_len = fopen("/sdcard/111.len", "wb");
#endif
    while (1) {
        fd_set rfds;
        struct timeval tv;
        int nfds, ret;
        MUTEX_LOCK(raop_rtp_mirror->run_mutex);
        if (!raop_rtp_mirror->running) {
            MUTEX_UNLOCK(raop_rtp_mirror->run_mutex);
            break;
        }
        MUTEX_UNLOCK(raop_rtp_mirror->run_mutex);
        /* Set timeout value to 5ms */
        tv.tv_sec = 0;
        tv.tv_usec = 5000;

        /* Get the correct nfds value and set rfds */
        FD_ZERO(&rfds);
        if (stream_fd == -1) {
            FD_SET(raop_rtp_mirror->mirror_data_sock, &rfds);
            nfds = raop_rtp_mirror->mirror_data_sock+1;
        } else {
            FD_SET(stream_fd, &rfds);
            nfds = stream_fd+1;
        }
        ret = select(nfds, &rfds, NULL, NULL, &tv);
        if (ret == 0) {
            /* Timeout happened */
            continue;
        } else if (ret == -1) {
            /* FIXME: Error happened */
            logger_log(raop_rtp_mirror->logger, LOGGER_INFO, "Error in select");
            break;
        }
        if (stream_fd == -1 && FD_ISSET(raop_rtp_mirror->mirror_data_sock, &rfds)) {
            struct sockaddr_storage saddr;
            socklen_t saddrlen;

            logger_log(raop_rtp_mirror->logger, LOGGER_INFO, "Accepting client");
            saddrlen = sizeof(saddr);
            stream_fd = accept(raop_rtp_mirror->mirror_data_sock, (struct sockaddr *)&saddr, &saddrlen);
            if (stream_fd == -1) {
                /* FIXME: Error happened */
                logger_log(raop_rtp_mirror->logger, LOGGER_INFO, "Error in accept %d %s", errno, strerror(errno));
                break;
            }
        }
        if (stream_fd != -1 && FD_ISSET(stream_fd, &rfds)) {
            // packetlen初始0
            ret = recv(stream_fd, packet + readstart, 4 - readstart, 0);
            if (ret == 0) {
                /* TCP socket closed */
                logger_log(raop_rtp_mirror->logger, LOGGER_INFO, "TCP socket closed");
                break;
            } else if (ret == -1) {
                /* FIXME: Error happened */
                logger_log(raop_rtp_mirror->logger, LOGGER_INFO, "Error in recv");
                break;
            }
            readstart += ret;
            if (readstart < 4) {
                continue;
            }
            if ((packet[0] == 80 && packet[1] == 79 && packet[2] == 83 && packet[3] == 84) || (packet[0] == 71 && packet[1] == 69 && packet[2] == 84)) {
                // POST或者GET
                logger_log(raop_rtp_mirror->logger, LOGGER_DEBUG, "handle http data");
            } else {
                // 普通数据块
                do {
                    // 读取剩下的124字节
                    ret = recv(stream_fd, packet + readstart, 128 - readstart, 0);
                    readstart = readstart + ret;
                } while (readstart < 128);
                int payloadsize = byteutils_get_int(packet, 0);
                // FIXME: 这里计算方式需要再确认
                short payloadtype = (short) (byteutils_get_short(packet, 4) & 0xff);
                short payloadoption = byteutils_get_short(packet, 6);

                // 处理内容数据
                if (payloadtype == 0) {
                    uint64_t payloadntp = byteutils_get_long(packet, 8);
                    // 读取时间
                    if (pts_base == 0) {
                        pts_base = ntptopts(payloadntp);
                    } else {
                        pts =  ntptopts(payloadntp) - pts_base;
                    }
                    // 这里是加密的数据
                    unsigned char* payload_in = malloc(payloadsize);
                    unsigned char* payload = malloc(payloadsize);
                    readstart = 0;
                    do {
                        // payload数据
                        ret = recv(stream_fd, payload_in + readstart, payloadsize - readstart, 0);
                        readstart = readstart + ret;
                    } while (readstart < payloadsize);
                    //logger_log(raop_rtp_mirror->logger, LOGGER_DEBUG, "readstart = %d", readstart);
#ifdef DUMP_H264
                    fwrite(payload_in, payloadsize, 1, file_source);
                    fwrite(&readstart, sizeof(readstart), 1, file_len);
#endif
                    // 解密数据
                    mirror_buffer_decrypt(raop_rtp_mirror->buffer, payload_in, payload, payloadsize);
                    int nalu_size = 0;
                    int nalu_num = 0;
                    while (nalu_size < payloadsize) {
                        int nc_len = (payload[nalu_size + 0] << 24) | (payload[nalu_size + 1] << 16) | (payload[nalu_size + 2] << 8) | (payload[nalu_size + 3]);
                        if (nc_len > 0) {
                            payload[nalu_size + 0] = 0;
                            payload[nalu_size + 1] = 0;
                            payload[nalu_size + 2] = 0;
                            payload[nalu_size + 3] = 1;
                            //int nalutype = payload[4] & 0x1f;
                            //logger_log(raop_rtp_mirror->logger, LOGGER_DEBUG, "nalutype = %d", nalutype);
                            nalu_size += nc_len + 4;
                            nalu_num++;
                        }
                    }
                    //logger_log(raop_rtp_mirror->logger, LOGGER_DEBUG, "nalu_size = %d, payloadsize = %d nalu_num = %d", nalu_size, payloadsize, nalu_num);

                    // 写入文件
#ifdef DUMP_H264
                    fwrite(payload, payloadsize, 1, file);
#endif
                    h264_decode_struct h264_data;
                    h264_data.data_len = payloadsize;
                    h264_data.data = payload;
                    h264_data.frame_type = 1;
                    h264_data.pts = pts;
                    raop_rtp_mirror->callbacks.video_process(raop_rtp_mirror->callbacks.cls, &h264_data);
                    free(payload_in);
                    free(payload);
                } else if ((payloadtype & 255) == 1) {
                    float mWidthSource = byteutils_get_float(packet, 40);
                    float mHeightSource = byteutils_get_float(packet, 44);
                    float mWidth = byteutils_get_float(packet, 56);
                    float mHeight =byteutils_get_float(packet, 60);
                    logger_log(raop_rtp_mirror->logger, LOGGER_DEBUG, "mWidthSource = %f mHeightSource = %f mWidth = %f mHeight = %f", mWidthSource, mHeightSource, mWidth, mHeight);
                    /*int mRotateMode = 0;

                    int p = payloadtype >> 8;
                    if (p == 4) {
                        mRotateMode = 1;
                    } else if (p == 7) {
                        mRotateMode = 3;
                    } else if (p != 0) {
                        mRotateMode = 2;
                    }*/

                    // sps_pps 这块数据是没有加密的
                    unsigned char payload[payloadsize];
                    readstart = 0;
                    do {
                        // payload数据
                        ret = recv(stream_fd, payload + readstart, payloadsize - readstart, 0);
                        readstart = readstart + ret;
                    } while (readstart < payloadsize);
                    h264codec_t h264;
                    h264.version = payload[0];
                    h264.profile_high = payload[1];
                    h264.compatibility = payload[2];
                    h264.level = payload[3];
                    h264.reserved6andNAL = payload[4];
                    h264.reserved3andSPS = payload[5];
                    h264.lengthofSPS = (short) (((payload[6] & 255) << 8) + (payload[7] & 255));
                    logger_log(raop_rtp_mirror->logger, LOGGER_DEBUG, "lengthofSPS = %d", h264.lengthofSPS);
                    h264.sequence = malloc(h264.lengthofSPS);
                    memcpy(h264.sequence, payload + 8, h264.lengthofSPS);
                    h264.numberOfPPS = payload[h264.lengthofSPS + 8];
                    h264.lengthofPPS = (short) (((payload[h264.lengthofSPS + 9] & 2040) + payload[h264.lengthofSPS + 10]) & 255);
                    h264.picture_parameter_set = malloc(h264.lengthofPPS);
                    logger_log(raop_rtp_mirror->logger, LOGGER_DEBUG, "lengthofPPS = %d", h264.lengthofPPS);
                    memcpy(h264.picture_parameter_set, payload + h264.lengthofSPS + 11, h264.lengthofPPS);
                    if (h264.lengthofSPS + h264.lengthofPPS < 102400) {
                        // 复制spspps
                        int sps_pps_len = (h264.lengthofSPS + h264.lengthofPPS) + 8;
                        //unsigned char sps_pps[sps_pps_len];
                        unsigned char* sps_pps = malloc(sps_pps_len * sizeof(unsigned char*));
                        memset(sps_pps,0,sps_pps_len);
                        sps_pps[0] = 0;
                        sps_pps[1] = 0;
                        sps_pps[2] = 0;
                        sps_pps[3] = 1;
                        memcpy(sps_pps + 4, h264.sequence, h264.lengthofSPS);
//                        int nalutype = sps_pps[4] & 0x1f;
//                        LOGV("sps  type = %d",nalutype);
//                        int w = 0;
//                        int h = 0;
//                        int fps = 0;
//                        int* wptr = &w;
//                        int* hptr = &h;
//                        int* fpsptr = &fps;
//                        h264_decode_sps(sps_pps,h264.lengthofSPS,wptr,hptr,fpsptr);//测试解析 sps 分辨率信息
                        sps_pps[h264.lengthofSPS + 4] = 0;
                        sps_pps[h264.lengthofSPS + 5] = 0;
                        sps_pps[h264.lengthofSPS + 6] = 0;
                        sps_pps[h264.lengthofSPS + 7] = 1;
                        memcpy(sps_pps + h264.lengthofSPS + 8, h264.picture_parameter_set, h264.lengthofPPS);
#ifdef DUMP_H264
                        fwrite(sps_pps, sps_pps_len, 1, file);
#endif
                        h264_decode_struct h264_data;
                        h264_data.data_len = sps_pps_len;
                        h264_data.data = sps_pps;
                        h264_data.frame_type = 0;
                        h264_data.pts = 0;
                        raop_rtp_mirror->callbacks.video_process(raop_rtp_mirror->callbacks.cls, &h264_data);
                    }
                    free(h264.picture_parameter_set);
                    free(h264.sequence);
                } else if (payloadtype == (short) 2) {
                    readstart = 0;
                    if (payloadsize > 0) {
                        unsigned char* payload_in = malloc(payloadsize);
                        do {
                            ret = recv(stream_fd, payload_in + readstart, payloadsize - readstart, 0);
                            readstart = readstart + ret;
                        } while (readstart < payloadsize);
                    }
                } else if (payloadtype == (short) 4) {
                    readstart = 0;
                    if (payloadsize > 0) {
                        unsigned char* payload_in = malloc(payloadsize);
                        do {
                            ret = recv(stream_fd, payload_in + readstart, payloadsize - readstart, 0);
                            readstart = readstart + ret;
                        } while (readstart < payloadsize);
                    }
                } else {
                    readstart = 0;
                    if (payloadsize > 0) {
                        unsigned char* payload_in = malloc(payloadsize);
                        do {
                            ret = recv(stream_fd, payload_in + readstart, payloadsize - readstart, 0);
                            readstart = readstart + ret;
                        } while (readstart < payloadsize);
                    }
                }
            }
            memset(packet, 0 , 128);
            readstart = 0;
        }
    }

    /* Close the stream file descriptor */
    if (stream_fd != -1) {
        closesocket(stream_fd);
    }
    logger_log(raop_rtp_mirror->logger, LOGGER_INFO, "Exiting TCP raop_rtp_mirror_thread thread");
#ifdef DUMP_H264
    fclose(file);
    fclose(file_source);
    fclose(file_len);
#endif
    return 0;
}

void
raop_rtp_start_mirror(raop_rtp_mirror_t *raop_rtp_mirror, int use_udp, unsigned short mirror_timing_rport, unsigned short * mirror_timing_lport,
                      unsigned short *mirror_data_lport)
{
    int use_ipv6 = 0;

    assert(raop_rtp_mirror);

    MUTEX_LOCK(raop_rtp_mirror->run_mutex);
    if (raop_rtp_mirror->running || !raop_rtp_mirror->joined) {
        MUTEX_UNLOCK(raop_rtp_mirror->run_mutex);
        return;
    }

    //raop_rtp_mirror->mirror_timing_rport = mirror_timing_rport;
    if (raop_rtp_mirror->remote_saddr.ss_family == AF_INET6) {
        use_ipv6 = 1;
    }
    use_ipv6 = 0;
    if (raop_rtp_init_mirror_sockets(raop_rtp_mirror, use_ipv6) < 0) {
        logger_log(raop_rtp_mirror->logger, LOGGER_INFO, "Initializing sockets failed");
        MUTEX_UNLOCK(raop_rtp_mirror->run_mutex);
        return;
    }
    if (mirror_timing_lport) *mirror_timing_lport = raop_rtp_mirror->mirror_timing_lport;
    if (mirror_data_lport) *mirror_data_lport = raop_rtp_mirror->mirror_data_lport;

    /* Create the thread and initialize running values */
    raop_rtp_mirror->running = 1;
    raop_rtp_mirror->joined = 0;

    THREAD_CREATE(raop_rtp_mirror->thread_mirror, raop_rtp_mirror_thread, raop_rtp_mirror);
    THREAD_CREATE(raop_rtp_mirror->thread_time, raop_rtp_mirror_thread_time, raop_rtp_mirror);
    MUTEX_UNLOCK(raop_rtp_mirror->run_mutex);
}

void raop_rtp_mirror_stop(raop_rtp_mirror_t *raop_rtp_mirror) {
    assert(raop_rtp_mirror);

    /* Check that we are running and thread is not
     * joined (should never be while still running) */
    MUTEX_LOCK(raop_rtp_mirror->run_mutex);
    if (!raop_rtp_mirror->running || raop_rtp_mirror->joined) {
        MUTEX_UNLOCK(raop_rtp_mirror->run_mutex);
        return;
    }
    raop_rtp_mirror->running = 0;
    MUTEX_UNLOCK(raop_rtp_mirror->run_mutex);

    /* Join the thread */
    THREAD_JOIN(raop_rtp_mirror->thread_mirror);

    MUTEX_LOCK(raop_rtp_mirror->time_mutex);
    COND_SIGNAL(raop_rtp_mirror->time_cond);
    MUTEX_UNLOCK(raop_rtp_mirror->time_mutex);

    THREAD_JOIN(raop_rtp_mirror->thread_time);
    if (raop_rtp_mirror->mirror_data_sock != -1) closesocket(raop_rtp_mirror->mirror_data_sock);
    if (raop_rtp_mirror->mirror_time_sock != -1) closesocket(raop_rtp_mirror->mirror_time_sock);

    /* Mark thread as joined */
    MUTEX_LOCK(raop_rtp_mirror->run_mutex);
    raop_rtp_mirror->joined = 1;
    MUTEX_UNLOCK(raop_rtp_mirror->run_mutex);
}

void raop_rtp_mirror_destroy(raop_rtp_mirror_t *raop_rtp_mirror) {
    if (raop_rtp_mirror) {
        raop_rtp_mirror_stop(raop_rtp_mirror);
        MUTEX_DESTROY(raop_rtp_mirror->run_mutex);
        MUTEX_DESTROY(raop_rtp_mirror->time_mutex);
        COND_DESTROY(raop_rtp_mirror->time_cond);
        mirror_buffer_destroy(raop_rtp_mirror->buffer);
    }
}

static int
raop_rtp_init_mirror_sockets(raop_rtp_mirror_t *raop_rtp_mirror, int use_ipv6)
{
    int dsock = -1, tsock = -1;
    unsigned short tport = 0, dport = 0;

    assert(raop_rtp_mirror);

    dsock = netutils_init_socket(&dport, use_ipv6, 0);
    tsock = netutils_init_socket(&tport, use_ipv6, 1);
    if (dsock == -1 || tsock == -1) {
        goto sockets_cleanup;
    }

    /* Listen to the data socket if using TCP */
    if (listen(dsock, 1) < 0)
        goto sockets_cleanup;


    /* Set socket descriptors */
    raop_rtp_mirror->mirror_data_sock = dsock;
    raop_rtp_mirror->mirror_time_sock = tsock;

    /* Set port values */
    raop_rtp_mirror->mirror_data_lport = dport;
    raop_rtp_mirror->mirror_timing_lport = tport;
    return 0;

    sockets_cleanup:
    if (tsock != -1) closesocket(tsock);
    if (dsock != -1) closesocket(dsock);
    return -1;
}

UINT Ue(BYTE *pBuff, UINT nLen, UINT* nStartBit) {
    //计算0bit的个数
    UINT nZeroNum = 0;
    while (*nStartBit < nLen * 8)
    {
        if (pBuff[*nStartBit / 8] & (0x80 >> (*nStartBit % 8))) //&:按位与，%取余
        {
            break;
        }
        nZeroNum++;
        (*nStartBit)++;
    }
    (*nStartBit) ++;


    //计算结果
    DWORD dwRet = 0;
    for (UINT i=0; i<nZeroNum; i++)
    {
        dwRet <<= 1;
        if (pBuff[*nStartBit / 8] & (0x80 >> (*nStartBit % 8)))
        {
            dwRet += 1;
        }
        (*nStartBit)++;
    }
    return (1 << nZeroNum) - 1 + dwRet;
}

int Se(BYTE *pBuff, UINT nLen, UINT* nStartBit) {
    int UeVal = Ue(pBuff, nLen, nStartBit);
    double k = UeVal;
    int nValue = ceil(k / 2);//ceil函数：ceil函数的作用是求不小于给定实数的最小整数。ceil(2)=ceil(1.2)=cei(1.5)=2.00
    if (UeVal % 2 == 0)
        nValue = -nValue;
    return nValue;
}

DWORD u(UINT BitCount,BYTE * buf,UINT* nStartBit) {
    DWORD dwRet = 0;
    for (UINT i=0; i<BitCount; i++)
    {
        dwRet <<= 1;
        if (buf[*nStartBit / 8] & (0x80 >> (*nStartBit % 8)))
        {
            dwRet += 1;
        }
        (*nStartBit)++;
    }
    return dwRet;

}

void de_emulation_prevention(BYTE* buf,unsigned int* buf_size) {
    int i=0,j=0;
    BYTE* tmp_ptr=NULL;
    unsigned int tmp_buf_size=0;
    int val=0;

    tmp_ptr=buf;
    tmp_buf_size=*buf_size;
    for(i=0;i<(tmp_buf_size-2);i++)
    {
        //check for 0x000003
        val=(tmp_ptr[i]^0x00) +(tmp_ptr[i+1]^0x00)+(tmp_ptr[i+2]^0x03);
        if(val==0)
        {
            //kick out 0x03
            for(j=i+2;j<tmp_buf_size-1;j++)
                tmp_ptr[j]=tmp_ptr[j+1];

            //and so we should devrease bufsize
            (*buf_size)--;
        }
    }

}


int h264_decode_sps(BYTE *buf, unsigned int nLen, int *width, int *height, int *fps) {
    UINT startss = 32;
    UINT* StartBit = &startss;
    *fps = 0;
    de_emulation_prevention(buf, &nLen);

    int forbidden_zero_bit = u(1, buf, StartBit);
    int nal_ref_idc = u(2, buf, StartBit);
    int nal_unit_type = u(5, buf, StartBit);
    if (nal_unit_type == 7) {
        int profile_idc = u(8, buf, StartBit);
        int constraint_set0_flag = u(1, buf, StartBit);//(buf[1] & 0x80)>>7;
        int constraint_set1_flag = u(1, buf, StartBit);//(buf[1] & 0x40)>>6;
        int constraint_set2_flag = u(1, buf, StartBit);//(buf[1] & 0x20)>>5;
        int constraint_set3_flag = u(1, buf, StartBit);//(buf[1] & 0x10)>>4;
        int reserved_zero_4bits = u(4, buf, StartBit);
        int level_idc = u(8, buf, StartBit);

        int seq_parameter_set_id = Ue(buf, nLen, StartBit);

        if (profile_idc == 100 || profile_idc == 110 ||
            profile_idc == 122 || profile_idc == 144) {
            int chroma_format_idc = Ue(buf, nLen, StartBit);
            if (chroma_format_idc == 3) {
                int residual_colour_transform_flag = u(1, buf, StartBit);
            }
            int bit_depth_luma_minus8 = Ue(buf, nLen, StartBit);
            int bit_depth_chroma_minus8 = Ue(buf, nLen, StartBit);
            int qpprime_y_zero_transform_bypass_flag = u(1, buf, StartBit);
            int seq_scaling_matrix_present_flag = u(1, buf, StartBit);

            int seq_scaling_list_present_flag[8];
            if (seq_scaling_matrix_present_flag) {
                for (int i = 0; i < 8; i++) {
                    seq_scaling_list_present_flag[i] = u(1, buf, StartBit);
                }
            }
        }
        int log2_max_frame_num_minus4 = Ue(buf, nLen, StartBit);
        int pic_order_cnt_type = Ue(buf, nLen, StartBit);
        if (pic_order_cnt_type == 0) {
            int log2_max_pic_order_cnt_lsb_minus4 = Ue(buf, nLen, StartBit);
        }
        else if (pic_order_cnt_type == 1) {
            int delta_pic_order_always_zero_flag = u(1, buf, StartBit);
            int offset_for_non_ref_pic = Se(buf, nLen, StartBit);
            int offset_for_top_to_bottom_field = Se(buf, nLen, StartBit);
            int num_ref_frames_in_pic_order_cnt_cycle = Ue(buf, nLen, StartBit);

            //int *offset_for_ref_frame = new int[num_ref_frames_in_pic_order_cnt_cycle];
            int* offset_for_ref_frame = malloc(num_ref_frames_in_pic_order_cnt_cycle * sizeof(int*));
            memset(offset_for_ref_frame,num_ref_frames_in_pic_order_cnt_cycle * sizeof(int*),0);

            for (int i = 0; i < num_ref_frames_in_pic_order_cnt_cycle; i++)
                offset_for_ref_frame[i] = Se(buf, nLen, StartBit);
            free(offset_for_ref_frame);
        }
        int num_ref_frames = Ue(buf, nLen, StartBit);
        int gaps_in_frame_num_value_allowed_flag = u(1, buf, StartBit);
        int pic_width_in_mbs_minus1 = Ue(buf, nLen, StartBit);
        int pic_height_in_map_units_minus1 = Ue(buf, nLen, StartBit);

        *width = (pic_width_in_mbs_minus1 + 1) * 16;
        *height = (pic_height_in_map_units_minus1 + 1) * 16;
        LOGV("width = %d   height = %d",*width,*height);

        int frame_mbs_only_flag = u(1, buf, StartBit);
        if (!frame_mbs_only_flag) {
            int mb_adaptive_frame_field_flag = u(1, buf, StartBit);
        }

        int direct_8x8_inference_flag = u(1, buf, StartBit);
        int frame_cropping_flag = u(1, buf, StartBit);
        if (frame_cropping_flag) {
            int frame_crop_left_offset = Ue(buf, nLen, StartBit);
            int frame_crop_right_offset = Ue(buf, nLen, StartBit);
            int frame_crop_top_offset = Ue(buf, nLen, StartBit);
            int frame_crop_bottom_offset = Ue(buf, nLen, StartBit);
        }
        int vui_parameter_present_flag = u(1, buf, StartBit);
        if (vui_parameter_present_flag) {
            int aspect_ratio_info_present_flag = u(1, buf, StartBit);
            if (aspect_ratio_info_present_flag) {
                int aspect_ratio_idc = u(8, buf, StartBit);
                if (aspect_ratio_idc == 255) {
                    int sar_width = u(16, buf, StartBit);
                    int sar_height = u(16, buf, StartBit);
                }
            }
            int overscan_info_present_flag = u(1, buf, StartBit);
            if (overscan_info_present_flag) {
                int overscan_appropriate_flagu = u(1, buf, StartBit);
            }
            int video_signal_type_present_flag = u(1, buf, StartBit);
            if (video_signal_type_present_flag) {
                int video_format = u(3, buf, StartBit);
                int video_full_range_flag = u(1, buf, StartBit);
                int colour_description_present_flag = u(1, buf, StartBit);
                if (colour_description_present_flag) {
                    int colour_primaries = u(8, buf, StartBit);
                    int transfer_characteristics = u(8, buf, StartBit);
                    int matrix_coefficients = u(8, buf, StartBit);
                }
            }
            int chroma_loc_info_present_flag = u(1, buf, StartBit);
            if (chroma_loc_info_present_flag) {
                int chroma_sample_loc_type_top_field = Ue(buf, nLen, StartBit);
                int chroma_sample_loc_type_bottom_field = Ue(buf, nLen, StartBit);
            }
            int timing_info_present_flag = u(1, buf, StartBit);

            if (timing_info_present_flag) {
                int num_units_in_tick = u(32, buf, StartBit);
                int time_scale = u(32, buf, StartBit);
                *fps = time_scale / num_units_in_tick;
                int fixed_frame_rate_flag = u(1, buf, StartBit);
                if (fixed_frame_rate_flag) {
                    *fps = *fps / 2;
                }
                LOGV("fps = %d",*fps);
            }
        }
        return 1;
    } else
        return 2;
}