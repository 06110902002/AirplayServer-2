//
// Created by Administrator on 2019/1/29/029.
//

#ifndef RAOP_RTP_MIRROR_H
#define RAOP_RTP_MIRROR_H

#include <stdint.h>
#include "raop.h"
#include "logger.h"

typedef struct raop_rtp_mirror_s raop_rtp_mirror_t;
typedef struct h264codec_s h264codec_t;
typedef  unsigned int UINT;
typedef  unsigned char BYTE;
typedef  unsigned long DWORD;

raop_rtp_mirror_t *raop_rtp_mirror_init(logger_t *logger, raop_callbacks_t *callbacks, const unsigned char *remote, int remotelen,
                                        const unsigned char *aeskey, const unsigned char *ecdh_secret, unsigned short timing_rport);
void raop_rtp_init_mirror_aes(raop_rtp_mirror_t *raop_rtp_mirror, uint64_t streamConnectionID);
void raop_rtp_start_mirror(raop_rtp_mirror_t *raop_rtp_mirror, int use_udp, unsigned short mirror_timing_rport, unsigned short * mirror_timing_lport,
                      unsigned short *mirror_data_lport);

static int raop_rtp_init_mirror_sockets(raop_rtp_mirror_t *raop_rtp_mirror, int use_ipv6);

void raop_rtp_mirror_stop(raop_rtp_mirror_t *raop_rtp_mirror);
void raop_rtp_mirror_destroy(raop_rtp_mirror_t *raop_rtp_mirror);

UINT Ue(BYTE *pBuff, UINT nLen, UINT* nStartBit);
int Se(BYTE *pBuff, UINT nLen, UINT* nStartBit);

DWORD u(UINT BitCount,BYTE * buf,UINT* nStartBit);
/**
 * H264的NAL起始码防竞争机制
 *
 * @param buf SPS数据内容
 *
 * @无返回值
 */
void de_emulation_prevention(BYTE* buf,unsigned int* buf_size);

/**
 * 解码SPS,获取视频图像宽、高和帧率信息
 *
 * @param buf SPS数据内容
 * @param nLen SPS数据的长度
 * @param width 图像宽度
 * @param height 图像高度
 * @成功则返回true , 失败则返回false
 */
int h264_decode_sps(BYTE * buf,unsigned int nLen,int *width,int *height,int *fps);
#endif //RAOP_RTP_MIRROR_H
