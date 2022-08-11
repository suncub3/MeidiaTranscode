//
// Created by suncube on 2022/8/9.
//

#ifndef SPEEDPLAY_SRC_TRANSCODE_H_
#define SPEEDPLAY_SRC_TRANSCODE_H_

#include <stdio.h>
#include <string>
#include <vector>
#include "glog/logging.h"

extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include <libavutil/channel_layout.h>
#include <libavutil/timestamp.h>
}

class Transcode
{
 public:
  Transcode();
  ~Transcode();
  // 对封装容器进行转化，比如MP4,MOV,FLV,TS等容器之间的转换
  void doTransmux(std::string srcPath, std::string dstPath);
  // 做转码(包括编码方式，码率，分辨率，采样率，采样格式等等的转换)
  void doTranscode(std::string sPath, std::string dPath);
 private:
  bool openFile(std::string src, std::string dst);
  bool checkCodec();
  bool initVideoDecoder();
  bool initAudioDecoder();
  bool initVideoEncoder();
  bool initAudioEncoder();
  void doDecodeVideo(AVPacket *packet);
  void doEncodeVideo(AVFrame *frame);
  void doDecodeAudio(AVPacket *packet);
  void doEncodeAudio(AVFrame *frame);
  void eraseVpkt();
  void eraseApkt();
  bool doWrite(AVPacket* packet,bool isVideo);
  void releaseSources();

  int video_in_stream_index_;
  int video_ou_stream_index_;
  int audio_in_stream_index_;
  int audio_ou_stream_index_;
  int64_t video_pts;
  int64_t audio_pts;

  AVFormatContext *inFmtCtx;
  AVFormatContext *outFmtCtx;
  uint8_t         **audio_buffer;
  int             left_size;
  bool            audio_init;

  // 用于音频解码
  AVCodecContext  *audio_de_ctx;
  // 用于视频解码
  AVCodecContext  *video_de_ctx;
  // 用于音频编码
  AVCodecContext  *audio_en_ctx;
  // 用于视频编码
  AVCodecContext  *video_en_ctx;
  // 用于音频格式转换
  SwrContext      *swr_ctx;
  // 用于视频格式转换
  SwsContext      *sws_ctx;
  bool            audio_need_convert;
  bool            video_need_convert;
  AVFrame         *video_de_frame;
  AVFrame         *audio_de_frame;
  AVFrame         *video_en_frame;
  AVFrame         *audio_en_frame;

  // 用于转码用的参数结构体
  typedef struct TranscodePar{
    bool copy;  // 是否和源文件保持一致
    union {
      enum AVCodecID codecId;
      enum AVPixelFormat pix_fmt;
      enum AVSampleFormat smp_fmt;
      int64_t i64_val;
      int     i32_val;
    } par_val;
  }TranscodePar;
#define Par_CodeId(copy,val) {copy,{.codecId=val}}
#define Par_PixFmt(copy,val) {copy,{.pix_fmt=val}}
#define Par_SmpFmt(copy,val) {copy,{.smp_fmt=val}}
#define Par_Int64t(copy,val) {copy,{.i64_val=val}}
#define Par_Int32t(copy,val) {copy,{.i32_val=val}}

  enum AVCodecID  src_video_id;
  enum AVCodecID  src_audio_id;
  TranscodePar    dst_video_id;
  TranscodePar    dst_audio_id;
  TranscodePar    dst_channel_layout;
  TranscodePar    dst_video_bit_rate;
  TranscodePar    dst_audio_bit_rate;
  TranscodePar    dst_sample_rate;
  TranscodePar    dst_width;
  TranscodePar    dst_height;
  TranscodePar    dst_fps;
  TranscodePar    dst_pix_fmt;
  TranscodePar    dst_sample_fmt;
  bool            video_need_transcode;
  bool            audio_need_transcode;

  std::vector<AVPacket *> videoCache;
  std::vector<AVPacket *> audioCache;
  int64_t last_video_pts;
  int64_t last_audio_pts;

  AVFrame *get_video_frame(enum AVPixelFormat pixfmt,int width,int height);
  AVFrame *get_audio_frame(enum AVSampleFormat smpfmt,int64_t ch_layout,int sample_rate,int nb_samples);
};

#endif //SPEEDPLAY_SRC_TRANSCODE_H_
