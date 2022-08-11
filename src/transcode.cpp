//
// Created by suncube on 2022/8/9.
//

#include "transcode.h"

Transcode::Transcode():inFmtCtx(nullptr),outFmtCtx(nullptr),
                       video_in_stream_index_(-1),video_ou_stream_index_(-1),audio_in_stream_index_(-1),audio_ou_stream_index_(-1),video_pts(0),audio_pts(0),
                       src_video_id(AV_CODEC_ID_NONE),src_audio_id(AV_CODEC_ID_NONE),
                       video_de_ctx(nullptr),audio_de_ctx(nullptr),video_en_ctx(nullptr),audio_en_ctx(nullptr),swr_ctx(nullptr),sws_ctx(nullptr),
                       video_de_frame(nullptr),audio_de_frame(nullptr),video_en_frame(nullptr),audio_en_frame(nullptr),
                       last_video_pts(0),last_audio_pts(0),audio_need_convert(false),video_need_convert(false),audio_init(false)
{

}

Transcode::~Transcode()
{
  releaseSources();
}

/** 实现MP4转换成MOV,FLV,TS,AVI文件，不改变编码方式
 */
void Transcode::doTransmux(std::string srcPath,std::string dstPath)
{
  if (!srcPath.length() || !dstPath.length()) {
    LOG(ERROR) << "path is null";
    return;
  }
  AVFormatContext *in_fmtCtx = nullptr,*ou_fmtCtx = nullptr;
  int video_in_index = -1,audio_in_index = -1;
  int video_ou_index = -1,audio_ou_index = -1;
  int ret = 0;
  if ((ret = avformat_open_input(&in_fmtCtx,srcPath.c_str(),nullptr,nullptr)) < 0) {
    LOG(ERROR) << "avformat_open_input fail " << av_err2str(ret);
    return;
  }
  if ((ret = avformat_find_stream_info(in_fmtCtx,nullptr)) < 0) {
    LOG(ERROR) << "avformat_find_stream_info fail " << av_err2str(ret);
    return;
  }
  av_dump_format(in_fmtCtx, 0, srcPath.c_str(), 0);

  if ((ret = avformat_alloc_output_context2(&ou_fmtCtx,nullptr,nullptr,dstPath.c_str())) < 0) {
    LOG(ERROR) << "avformat_alloc_output_context2 fail " << av_err2str(ret);
    return;
  }

  for (int i=0; i<in_fmtCtx->nb_streams; i++) {
    AVStream *in_stream = in_fmtCtx->streams[i];
    AVStream *out_stream = avformat_new_stream(ou_fmtCtx,nullptr);

    if (!out_stream) {
      DLOG(ERROR) <<"Failed allocating output stream";
      return;
    }

    if (in_stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && video_in_index == -1) {
      video_in_index = i;
      video_ou_index = out_stream->index;
    }

    if (in_stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && audio_in_index == -1) {
      audio_in_index = i;
      audio_ou_index = out_stream->index;
    }
    if ((ret = avcodec_parameters_copy(out_stream->codecpar, in_stream->codecpar)) <0) {
      DLOG(ERROR) <<"avcodec_parameters_copy fail " << av_err2str(ret);
      return;
    }

    uint32_t src_codec_tag = in_stream->codecpar->codec_tag;
    if (av_codec_get_id(ou_fmtCtx->oformat->codec_tag, src_codec_tag) != out_stream->codecpar->codec_id) {
      out_stream->codecpar->codec_tag = 0;
    }
  }

  // 当flags没有AVFMT_NOFILE标记的时候才能调用avio_open2()函数进行初始化
  if (!(ou_fmtCtx->flags & AVFMT_NOFILE)) {
    if ((ret = avio_open2(&ou_fmtCtx->pb,dstPath.c_str(),AVIO_FLAG_WRITE,nullptr,nullptr)) < 0) {
      LOG(ERROR) << "avio_open2 failed " << ret;
      return;
    }
  }

  // 写入文件头信息
  if ((ret = avformat_write_header(ou_fmtCtx, nullptr)) < 0) {
    LOG(ERROR) << "avformat_write_header failed " << ret;
    return;
  }
  av_dump_format(ou_fmtCtx, 0, dstPath.c_str(), 1);

  AVPacket *pkt = av_packet_alloc();
  while ((av_read_frame(in_fmtCtx, pkt)) == 0) {
    if (pkt->stream_index != video_in_index && pkt->stream_index != audio_in_index) {
      continue;
    }

    AVStream *in_stream = in_fmtCtx->streams[pkt->stream_index];
    AVStream *ou_stream = in_stream->index == video_in_index ? ou_fmtCtx->streams[video_ou_index] : ou_fmtCtx->streams[audio_ou_index];

    // 每个packet中pts,dts,duration 转换成浮点数时间的公式(以pts为例)：pts * timebase.num/timebase.den
    av_packet_rescale_ts(pkt, in_stream->time_base, ou_stream->time_base);
    pkt->stream_index = ou_stream->index;

    if (av_interleaved_write_frame(ou_fmtCtx, pkt) < 0) {
      DLOG(ERROR) <<"Error muxing packet";
      return;
    }
    av_packet_unref(pkt);
  }
  av_packet_free(&pkt);
  av_write_trailer(ou_fmtCtx);
  avformat_close_input(&in_fmtCtx);
  avio_close(ou_fmtCtx->pb);
  avformat_free_context(ou_fmtCtx);
}

/** 做转码(包括编码方式，码率，分辨率，采样率，采样格式等等的转换)
 */
void Transcode::doTranscode(std::string sPath,std::string dPath)
{
  int ret = 0;
  // 做编码方式的转码，目标封装格式要支持 第一个参数代表是否引用源文件的参数，如果为true则忽略第二个参数，否则使用第二个参数
  // 视频
  dst_video_id = Par_CodeId(true,AV_CODEC_ID_H264);
  dst_width =  Par_Int32t(true,1920);
  dst_height = Par_Int32t(true,1080);
  dst_video_bit_rate = Par_Int64t(true,900000);       // 0.9Mb/s
  dst_pix_fmt = Par_PixFmt(true,AV_PIX_FMT_YUV420P);
  dst_fps = Par_Int32t(true,25);  //fps
  video_need_transcode = true;

  // 音频
  dst_audio_id =  Par_CodeId(false,AV_CODEC_ID_AAC);
  dst_audio_bit_rate = Par_Int64t(true,128000);
  dst_sample_rate = Par_Int32t(false,48000);    // 44.1khz
  dst_sample_fmt = Par_SmpFmt(false,AV_SAMPLE_FMT_FLTP);
  dst_channel_layout = Par_Int64t(false,AV_CH_LAYOUT_STEREO);   // 双声道
  audio_need_transcode = true;

  // 打开输入文件及输出文件的上下文
  if (!openFile(sPath, dPath)) {
    LOG(ERROR) << "open file failed";
    return;
  }
  
  if (!checkCodec()) {
    return;
  }

  // 为输出文件添加视频流
  if (video_in_stream_index_ != -1 && video_need_transcode) {
    if (!initVideoDecoder()) {
      LOG(ERROR) << "init Video Decoder failed";
      return;
    }
    if (!initVideoEncoder()) {
      LOG(ERROR) << "init Video Encoder failed";
      return;
    }
  }

  // 为输出文件添加音频流
  if (audio_in_stream_index_ != -1 && audio_need_transcode) {
    if (!initAudioDecoder()) {
      LOG(ERROR) << "init Audio Decoder failed";
      return;
    }
    if (!initAudioEncoder()) {
      LOG(ERROR) << "init Audio Encoder failed";
      return;
    }
  }

  av_dump_format(outFmtCtx, 0, dPath.c_str(), 1);

  // 打开解封装的上下文
  if (!(outFmtCtx->oformat->flags & AVFMT_NOFILE)) {
    if (avio_open(&outFmtCtx->pb, dPath.c_str(), AVIO_FLAG_WRITE) < 0) {
      LOG(ERROR) << "avio_open fail";
      return;
    }
  }

  if ((ret = avformat_write_header(outFmtCtx, nullptr)) < 0) {
    LOG(ERROR) << "avformat_write_header fail" << ret;
    return;
  }

  // 读取源文件中的音视频数据进行解码
  AVPacket *inPacket = av_packet_alloc();
  while (av_read_frame(inFmtCtx, inPacket) == 0) {
    // 说明读取的视频数据
    if (inPacket->stream_index == video_in_stream_index_ && video_need_transcode) {
      doDecodeVideo(inPacket);
    }

    // 说明读取的音频数据
    if (inPacket->stream_index == audio_in_stream_index_ && audio_need_transcode) {
      doDecodeAudio(inPacket);
    }
    av_packet_unref(inPacket);
  }
  av_packet_free(&inPacket);

  // 刷新解码缓冲区，获取缓冲区中数据
  if (video_in_stream_index_ != -1 && video_need_transcode) {
    doDecodeVideo(nullptr);
  }
  if (audio_in_stream_index_ != -1 && audio_need_transcode) {
    doDecodeAudio(nullptr);
  }

  while (!videoCache.empty() || !audioCache.empty()) {
    doWrite(nullptr, true);
  }

  // 写入尾部信息
  if (outFmtCtx) {
    av_write_trailer(outFmtCtx);
  }
}

bool Transcode::openFile(std::string srcPath, std::string dstPath)
{
  if (!srcPath.length() || !dstPath.length()) {
    LOG(ERROR) << "path is null";
    return false;
  }
  int ret = 0;
  // 打开输入文件
  if (avformat_open_input(&inFmtCtx, srcPath.c_str(), nullptr, nullptr) < 0) {
    DLOG(ERROR) <<"avformat_open_input failed:" << srcPath;
    return false;
  }
  if ((ret = avformat_find_stream_info(inFmtCtx, nullptr)) < 0) {
    LOG(ERROR) << "avformat_find_stream_info fail " << ret;
    return false;
  }
  // 输出输入文件信息
  av_dump_format(inFmtCtx,0,srcPath.c_str(),0);

  for (int i=0; i<inFmtCtx->nb_streams; i++) {
    AVCodecParameters *codecpar = inFmtCtx->streams[i]->codecpar;
    if(codecpar->codec_type == AVMEDIA_TYPE_VIDEO && video_in_stream_index_ == -1) {
      src_video_id = codecpar->codec_id;
      video_in_stream_index_ = i;
    }
    if (codecpar->codec_type == AVMEDIA_TYPE_AUDIO && audio_in_stream_index_ == -1) {
      src_audio_id = codecpar->codec_id;
      audio_in_stream_index_ = i;
    }
  }

  // 打开输出流
  if (avformat_alloc_output_context2(&outFmtCtx, nullptr, nullptr, dstPath.c_str()) < 0) {
    LOG(ERROR) << "avformat_alloc_context fail";
    return false;
  }
  return true;
}

bool Transcode::checkCodec() {
  // 检查目标编码方式是否被支持
  if (video_in_stream_index_ != -1 && av_codec_get_tag(outFmtCtx->oformat->codec_tag, dst_video_id.par_val.codecId) == 0) {
    LOG(ERROR) << "video tag not found";
    return false;
  }
  if (audio_in_stream_index_ != -1 && av_codec_get_tag(outFmtCtx->oformat->codec_tag,dst_audio_id.par_val.codecId) == 0) {
    LOG(ERROR) << "audio tag not found";
    return false;
  }
  return true;
}

static int select_sample_rate(AVCodec *codec,int rate)
{
  int best_rate = 0;
  int default_rate = 44100;
  const int* p = codec->supported_samplerates;
  if (!p) {
    return default_rate;
  }
  while (*p) {
    best_rate = *p;
    if (*p == rate) {
      break;
    }
    p++;
  }

  if (best_rate != rate && best_rate != 0 && best_rate != default_rate) {
    return default_rate;
  }

  return best_rate;
}

static enum AVSampleFormat select_sample_format(AVCodec *codec,enum AVSampleFormat fmt)
{
  enum AVSampleFormat ret_fmt = AV_SAMPLE_FMT_NONE;
  enum AVSampleFormat default_fmt = AV_SAMPLE_FMT_FLTP;
  const enum AVSampleFormat * fmts = codec->sample_fmts;
  if (!fmts) {
    return default_fmt;
  }
  while (*fmts != AV_SAMPLE_FMT_NONE) {
    ret_fmt = *fmts;
    if (ret_fmt == fmt) {
      break;
    }
    fmts++;
  }

  if (ret_fmt != fmt && ret_fmt != AV_SAMPLE_FMT_NONE && ret_fmt != default_fmt) {
    return default_fmt;
  }

  return ret_fmt;
}

static int64_t select_channel_layout(AVCodec *codec,int64_t channel_layout)
{
  int64_t ret_channel = 0;
  int64_t default_channel = AV_CH_LAYOUT_STEREO;
  const uint64_t * channels = codec->channel_layouts;
  if (!channels) {
    return default_channel;
  }
  while (*channels) {
    ret_channel = *channels;
    if (ret_channel == channel_layout) {
      break;
    }
    channels++;
  }

  if (ret_channel != channel_layout && ret_channel != AV_SAMPLE_FMT_NONE && ret_channel != default_channel) {
    return default_channel;
  }

  return ret_channel;
}

static enum AVPixelFormat select_pixel_format(AVCodec *codec,enum AVPixelFormat fmt) {
  enum AVPixelFormat ret_pixfmt = AV_PIX_FMT_NONE;
  enum AVPixelFormat default_fmt = AV_PIX_FMT_YUV420P;
  const enum AVPixelFormat *fmts = codec->pix_fmts;
  if (!fmts) {
    return default_fmt;
  }
  while (*fmts != AV_PIX_FMT_NONE) {
    ret_pixfmt = *fmts;
    if (ret_pixfmt == fmt) {
      break;
    }
    fmts++;
  }

  if (ret_pixfmt != fmt && ret_pixfmt != AV_PIX_FMT_NONE && ret_pixfmt != default_fmt) {
    return default_fmt;
  }

  return ret_pixfmt;
}

bool Transcode::initVideoDecoder() {
  if (src_video_id == AV_CODEC_ID_NONE) {
    LOG(ERROR) << "src do not has video";
    return false;
  }
  AVCodec *codec = avcodec_find_decoder(src_video_id);
  video_de_ctx = avcodec_alloc_context3(codec);
  if (video_de_ctx == nullptr) {
    LOG(ERROR) << "video decodec context create fail";
    return false;
  }

  // 设置解码参数;这里是直接从AVCodecParameters中拷贝
  AVCodecParameters *codecpar = inFmtCtx->streams[video_in_stream_index_]->codecpar;
  if (avcodec_parameters_to_context(video_de_ctx, codecpar) < 0) {
    LOG(ERROR) << "avcodec_parameters_to_context fail";
    return false;
  }
  // 初始化解码器
  if (avcodec_open2(video_de_ctx, codec, nullptr) < 0) {
    LOG(ERROR) << "video avcodec_open2 fail";
    return false;
  }
  return true;
}

bool Transcode::initAudioDecoder() {
  if (src_audio_id == AV_CODEC_ID_NONE) {
    LOG(ERROR) << "src do not has audio";
    return false;
  }
  AVCodec *codec = avcodec_find_decoder(src_audio_id);
  if (!codec) {
    LOG(ERROR) << "audio decoder not found";
    return false;
  }
  audio_de_ctx = avcodec_alloc_context3(codec);
  if (!audio_de_ctx) {
    LOG(ERROR) << "audio decodec_ctx fail";
    return false;
  }

  // 设置音频解码上下文参数;这里来自于源文件的拷贝
  if (avcodec_parameters_to_context(audio_de_ctx, inFmtCtx->streams[audio_in_stream_index_]->codecpar) < 0) {
    LOG(ERROR) << "audio set decodec ctx fail";
    return false;
  }

  if ((avcodec_open2(audio_de_ctx, codec, nullptr)) < 0) {
    LOG(ERROR) << "audio avcodec_open2 fail";
    return false;
  }
  return true;
}

bool Transcode::initVideoEncoder()
{
  // 创建视频输出流
  AVStream* stream = avformat_new_stream(outFmtCtx, nullptr);
  if (!stream) {
    LOG(ERROR) << "avformat_new_stream fail";
    return false;
  }
  video_ou_stream_index_ = stream->index;
  AVCodecParameters *inpar = inFmtCtx->streams[video_in_stream_index_]->codecpar;

  // find encoder
  AVCodec *codec = avcodec_find_encoder(dst_video_id.par_val.codecId);
  if (codec == nullptr) {
    LOG(ERROR) << "video codec not found";
    return false;
  }

  video_en_ctx = avcodec_alloc_context3(codec);
  if (video_en_ctx == nullptr) {
    LOG(ERROR) << "alloc video codec failed";
    return false;
  }

  // 设置编码相关参数
  video_en_ctx->codec_id = codec->id;
  // 设置码率
  video_en_ctx->bit_rate = dst_video_bit_rate.copy?0:dst_video_bit_rate.par_val.i64_val;
  // 设置视频宽
  video_en_ctx->width = dst_width.copy?inpar->width:dst_width.par_val.i32_val;
  // 设置视频高
  video_en_ctx->height = dst_height.copy?inpar->height:dst_height.par_val.i32_val;
  // 设置Rational
  int frame_num = dst_fps.copy ? inFmtCtx->streams[video_in_stream_index_]->r_frame_rate.num : 1;
  int frame_den = dst_fps.copy ? inFmtCtx->streams[video_in_stream_index_]->r_frame_rate.den : dst_fps.par_val.i32_val;

  video_en_ctx->framerate = (AVRational){frame_num,frame_den};
  // 设置时间基;
  video_en_ctx->time_base = (AVRational){frame_den,frame_num};
  // I帧间隔，决定了压缩率
  video_en_ctx->gop_size = 10000;
  // 设置视频像素格式
  enum AVPixelFormat want_pix_fmt = dst_pix_fmt.copy?(AVPixelFormat)inpar->format:dst_pix_fmt.par_val.pix_fmt;
  enum AVPixelFormat result_pix_fmt = select_pixel_format(codec, want_pix_fmt);
  if (result_pix_fmt == AV_PIX_FMT_NONE) {
    LOG(ERROR) << "can not find support pixel format";
    return false;
  }
  video_en_ctx->pix_fmt = result_pix_fmt;

  // 每组I帧间B帧的最大个数
  if (codec->id == AV_CODEC_ID_MPEG2VIDEO) {
    video_en_ctx->max_b_frames = 2;
  }
  /* Needed to avoid using macroblocks in which some coeffs overflow.
  * This does not happen with normal video, it just happens here as
  * the motion of the chroma plane does not match the luma plane. */
  if (codec->id == AV_CODEC_ID_MPEG1VIDEO) {
    video_en_ctx->mb_decision = 2;
  }

  // 判断是否需要进行格式转换
  if (result_pix_fmt != (enum AVPixelFormat)inpar->format || video_en_ctx->width != inpar->width || video_en_ctx->height != inpar->height) {
    video_need_convert = true;
  }

  // 对应一些封装器需要添加这个标记
  /** 遇到问题：生成的mp4或者mov文件没有显示用于预览的小图片
   *  分析原因：之前代编码器器flags标记设置的为AVFMT_GLOBALHEADER(错了)，正确的值应该是AV_CODEC_FLAG_GLOBAL_HEADER
   *  解决思路：使用如下代码设置AV_CODEC_FLAG_GLOBAL_HEADER
   */
  if (outFmtCtx->oformat->flags & AVFMT_GLOBALHEADER) {
    video_en_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  }

  // x264编码特有参数
  if (video_en_ctx->codec_id == AV_CODEC_ID_H264) {
    av_opt_set(video_en_ctx->priv_data,"preset","superfast",0);
    av_opt_set(video_en_ctx->priv_data, "tune", "zerolatency", 0);
    av_opt_set(video_en_ctx->priv_data, "threads", "auto", 0);
    video_en_ctx->flags |= AV_CODEC_FLAG2_LOCAL_HEADER;
  }

  int ret = 0;
  if ((ret = avcodec_open2(video_en_ctx, video_en_ctx->codec, nullptr)) < 0) {
    LOG(ERROR) << "video encode open2() fail " << ret;
    return false;
  }

  if((ret = avcodec_parameters_from_context(stream->codecpar, video_en_ctx)) < 0) {
    LOG(ERROR) << "video avcodec_parameters_from_context fail";
    return false;
  }
  stream->time_base = (AVRational){1,video_en_ctx->framerate.num};

  if (video_need_convert) {
    // 分配像素转换上下文
    if (sws_ctx == nullptr) {
      sws_ctx = sws_getContext(inpar->width, inpar->height, (enum AVPixelFormat) inpar->format,
                               video_en_ctx->width, video_en_ctx->height, video_en_ctx->pix_fmt, SWS_BICUBIC,
                               nullptr, nullptr, nullptr);
      if (!sws_ctx) {
        LOG(ERROR) << "sws_getContext fail";
        return false;
      }
    }
  }

  return true;
}

bool Transcode::initAudioEncoder()
{
  // 创建音频输出流
  AVStream *stream = avformat_new_stream(outFmtCtx, nullptr);
  if (!stream) {
    LOG(ERROR) << "avformat_new_stream fail";
    return false;
  }
  audio_ou_stream_index_ = stream->index;
  AVCodecParameters *incodecpar = inFmtCtx->streams[audio_in_stream_index_]->codecpar;

  AVCodec *codec = avcodec_find_encoder(dst_audio_id.par_val.codecId);
  audio_en_ctx = avcodec_alloc_context3(codec);
  if (audio_en_ctx == nullptr) {
    LOG(ERROR) << "audio codec ctx nullptr";
    return false;
  }
  // 设置音频编码参数
  // 采样率
  int want_sample_rate = dst_sample_rate.copy?incodecpar->sample_rate:dst_sample_rate.par_val.i32_val;
  int result_sample_rate = select_sample_rate(codec, want_sample_rate);
  if (result_sample_rate == 0) {
    LOG(ERROR) << "cannot surpot sample_rate";
    return false;
  }
  audio_en_ctx->sample_rate = result_sample_rate;
  // 采样格式
  enum AVSampleFormat want_sample_fmt = dst_sample_fmt.copy?(enum AVSampleFormat)incodecpar->format:dst_sample_fmt.par_val.smp_fmt;
  enum AVSampleFormat result_sample_fmt = select_sample_format(codec, want_sample_fmt);
  if (result_sample_fmt == AV_SAMPLE_FMT_NONE) {
    //LOGD("cannot surpot sample_fmt");
    releaseSources();
    return false;
  }
  audio_en_ctx->sample_fmt  = result_sample_fmt;
  // 声道格式
  int64_t want_channel = dst_channel_layout.copy?incodecpar->channel_layout:dst_channel_layout.par_val.i64_val;
  int64_t result_channel = select_channel_layout(codec, want_channel);
  if (!result_channel) {
    LOG(ERROR) << "cannot surpot channel_layout";
    return false;
  }
  audio_en_ctx->channel_layout = result_channel;
  // 声道数
  audio_en_ctx->channels = av_get_channel_layout_nb_channels(audio_en_ctx->channel_layout);
  // 编码后的码率
  audio_en_ctx->bit_rate = dst_audio_bit_rate.copy?incodecpar->bit_rate:dst_audio_bit_rate.par_val.i32_val;
  // 设置时间基
  audio_en_ctx->time_base = (AVRational){1,audio_en_ctx->sample_rate};

  // 设置编码的相关标记，这样进行封装的时候不会漏掉一些元信息
  if (outFmtCtx->oformat->flags & AVFMT_GLOBALHEADER) {
    audio_en_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  }

  // 初始化编码器
  if (avcodec_open2(audio_en_ctx, codec, nullptr) < 0) {
    LOG(ERROR) << "audio avcodec_open2() fail";
    return false;
  }

  if (audio_en_ctx->frame_size != incodecpar->frame_size || result_sample_fmt != incodecpar->format || result_channel != incodecpar->channel_layout || result_sample_rate != incodecpar->sample_rate) {
    audio_need_convert = true;
  }

  int ret = 0;
  // 将编码信息设置到音频流中
  if ((ret = avcodec_parameters_from_context(stream->codecpar, audio_en_ctx)) < 0) {
    LOG(ERROR) << "audio copy audio stream fail";
    return false;
  }
  stream->time_base = audio_en_ctx->time_base;

  // 初始化重采样
  if (audio_need_convert) {
    swr_ctx = swr_alloc_set_opts(nullptr, audio_en_ctx->channel_layout, audio_en_ctx->sample_fmt,
                                 audio_en_ctx->sample_rate, incodecpar->channel_layout,
                                 (enum AVSampleFormat)incodecpar->format, incodecpar->sample_rate, 0, nullptr);
    if ((ret = swr_init(swr_ctx)) < 0) {
      LOG(ERROR) << "swr_alloc_set_opts() fail " << ret;
      return false;
    }
  }

  return true;
}

void Transcode::doDecodeVideo(AVPacket *inpacket)
{
  int ret = 0;
  if (video_de_ctx == nullptr) {
    if (!initVideoDecoder()) {
      LOG(ERROR) << "initVideoDecoder fail";
      return;
    }
  }

  if (video_de_frame == nullptr) {
    video_de_frame = av_frame_alloc();
  }

  if ((ret = avcodec_send_packet(video_de_ctx,inpacket)) < 0) {
    LOG(ERROR) << "video avcodec_send_packet fail " << av_err2str(ret);
    return;
  }
  while (true) {
    // 从解码缓冲区接收解码后的数据
    if((ret = avcodec_receive_frame(video_de_ctx, video_de_frame)) < 0) {
      if (ret == AVERROR_EOF) {
        LOG(INFO) << "audio decode finish";
        doEncodeVideo(nullptr);
      }
      break;
    }

    // 如果需要格式转换 则在这里进行格式转换
    if (video_en_frame == nullptr) {
      video_en_frame = get_video_frame(video_en_ctx->pix_fmt, video_en_ctx->width, video_en_ctx->height);
      if (video_en_frame == nullptr) {
        LOG(ERROR) << "can't get video frame";
        return;
      }
    }

    if (video_need_convert) {
      // 进行转换;返回目标height
      ret = sws_scale(sws_ctx, video_de_frame->data, video_de_frame->linesize, 0, video_de_frame->height, video_en_frame->data, video_en_frame->linesize);
      if (ret < 0) {
        LOG(ERROR) << "sws_scale fail";
        return;
      }
    } else {
      // 直接拷贝即可
      av_frame_copy(video_en_frame, video_de_frame);
    }
//    LOG(INFO) << "video_de_frame pts: " << video_de_frame->pts;
    // 这里pts的timebase 采用video_en_ctx->time_base 即为{1, fps}
    video_en_frame->pts = video_pts;
    video_pts++;
    doEncodeVideo(video_en_frame);
  }
}

void Transcode::doEncodeVideo(AVFrame *frame)
{
  int ret = 0;
  // 获得了解码后的数据，然后进行重新编码
  if ((ret = avcodec_send_frame(video_en_ctx, frame)) < 0) {
    LOG(ERROR) << "video encode avcodec_send_frame fail";
    return;
  }

  while (true) {
    AVPacket *pkt = av_packet_alloc();
    if((ret = avcodec_receive_packet(video_en_ctx, pkt)) < 0) {
      break;
    }

    AVStream *stream = outFmtCtx->streams[video_ou_stream_index_];
    av_packet_rescale_ts(pkt, video_en_ctx->time_base, stream->time_base);
    pkt->stream_index = video_ou_stream_index_;

//    LOG(INFO) << "out pkt pts: " << pkt->pts;
    doWrite(pkt, true);
  }
}

void Transcode::doDecodeAudio(AVPacket *packet)
{
  int ret = 0;
  if (audio_de_ctx == nullptr) {
    if (!initAudioDecoder()) {
      LOG(ERROR) << "initAudioDecoder fail";
      return;
    }
  }

  // 创建解码用的AVFrame
  if (!audio_de_frame) {
    audio_de_frame = av_frame_alloc();
  }

  if ((ret = avcodec_send_packet(audio_de_ctx, packet)) < 0) {
    LOG(ERROR) << "audio avcodec_send_packet fail " << ret;
    return;
  }

  while (true) {
    if ((ret = avcodec_receive_frame(audio_de_ctx, audio_de_frame)) < 0) {
      if (ret == AVERROR_EOF) {
        LOG(INFO) << "audio decode finish";
        doEncodeAudio(nullptr);
      }
      break;
    }

    // 创建编码器用的AVFrame
    if (audio_en_frame == nullptr) {
      audio_en_frame = get_audio_frame(audio_en_ctx->sample_fmt, audio_en_ctx->channel_layout, audio_en_ctx->sample_rate, audio_en_ctx->frame_size);
      if (audio_en_frame == nullptr) {
        LOG(ERROR) << "can not create audio frame";
        return;
      }
    }

    // 解码成功，然后再重新进行编码;
    // 为了避免数据污染，所以这里只需要要将解码后得到的AVFrame中data数据拷贝到编码用的audio_en_frame中，解码后的其它数据则丢弃
    if (audio_need_convert) {
      if (!audio_init) {
        ret = av_samples_alloc(audio_buffer, audio_en_frame->linesize,audio_en_frame->channels,
                               audio_en_frame->nb_samples,(enum AVSampleFormat)audio_en_frame->format, 0);
        audio_init = true;
        left_size = 0;
      }
      /** 遇到问题：当音频编码方式不一致时转码后无声音
       *  分析原因：因为每个编码方式对应的AVFrame中的nb_samples不一样，所以再进行编码前要进行AVFrame的转换
       *  解决方案：进行编码前先转换
       */
      bool first = true;
      while (true) {
        // 进行转换
        if (first) {
          ret = swr_convert(swr_ctx, audio_buffer, audio_en_frame->nb_samples, (const uint8_t**)audio_de_frame->data, audio_de_frame->nb_samples);
          if (ret < 0) {
            LOG(ERROR) << "swr_convert() fail";
            return;
          }
          first = false;

          int use = ret-left_size >= 0 ?ret - left_size:ret;
          int size = av_get_bytes_per_sample((enum AVSampleFormat)audio_en_frame->format);
          for (int ch=0; ch<audio_en_frame->channels; ch++) {
            for (int i = 0; i<use; i++) {
              audio_en_frame->data[ch][(i+left_size)*size] = audio_buffer[ch][i*size];
              audio_en_frame->data[ch][(i+left_size)*size+1] = audio_buffer[ch][i*size+1];
              audio_en_frame->data[ch][(i+left_size)*size+2] = audio_buffer[ch][i*size+2];
              audio_en_frame->data[ch][(i+left_size)*size+3] = audio_buffer[ch][i*size+3];
            }
          }
          // 编码
          left_size += ret;
          if (left_size >= audio_en_frame->nb_samples) {
            left_size -= audio_en_frame->nb_samples;
            // 编码
            audio_en_frame->pts = av_rescale_q(audio_pts, (AVRational){1,audio_en_frame->sample_rate}, audio_en_ctx->time_base);
            audio_pts += audio_en_frame->nb_samples;
            doEncodeAudio(audio_en_frame);

            if (left_size > 0) {
              int size = av_get_bytes_per_sample((enum AVSampleFormat)audio_en_frame->format);
              for (int ch=0; ch<audio_en_frame->channels; ch++) {
                for (int i = 0; i<left_size; i++) {
                  audio_en_frame->data[ch][i*size] = audio_buffer[ch][(use+i)*size];
                  audio_en_frame->data[ch][i*size+1] = audio_buffer[ch][(use+i)*size+1];
                  audio_en_frame->data[ch][i*size+2] = audio_buffer[ch][(use+i)*size+2];
                  audio_en_frame->data[ch][i*size+3] = audio_buffer[ch][(use+i)*size+3];
                }
              }
            }
          }
        } else {
          ret = swr_convert(swr_ctx, audio_buffer, audio_en_frame->nb_samples, nullptr, 0);
          if (ret < 0) {
            LOG(ERROR) << "swr_convert() fail";
            return;
          }
          int size = av_get_bytes_per_sample((enum AVSampleFormat)audio_en_frame->format);
          for (int ch=0; ch<audio_en_frame->channels; ch++) {
            for (int i = 0; i < ret && i+left_size < audio_en_frame->nb_samples; i++) {
              audio_en_frame->data[ch][(left_size + i)*size] = audio_buffer[ch][i*size];
              audio_en_frame->data[ch][(left_size + i)*size + 1] = audio_buffer[ch][i*size + 1];
              audio_en_frame->data[ch][(left_size + i)*size + 2] = audio_buffer[ch][i*size + 2];
              audio_en_frame->data[ch][(left_size + i)*size + 3] = audio_buffer[ch][i*size + 3];
            }
          }
          left_size += ret;
          if (left_size >= audio_en_frame->nb_samples) {
            left_size -= audio_en_frame->nb_samples;
            // 编码
            audio_en_frame->pts = av_rescale_q(audio_pts, (AVRational){1,audio_en_frame->sample_rate}, audio_en_ctx->time_base);
            audio_pts += audio_en_frame->nb_samples;
            doEncodeAudio(audio_en_frame);
          } else {
            break;
          }
        }
      }

    } else {
      av_frame_copy(audio_en_frame, audio_de_frame);
      int frame_size = audio_en_frame->nb_samples;
      /** 遇到问题：得到的文件播放时音画不同步
       *  分析原因：由于音频的AVFrame的pts没有设置对；pts是基于AVCodecContext的时间基的时间，所以pts的设置公式：
       *  音频：pts  = (timebase.den/sample_rate)*nb_samples*index；  index为当前第几个音频AVFrame(索引从0开始)，nb_samples为每个AVFrame中的采样数
       *  视频：pts = (timebase.den/fps)*index；
       *  解决方案：按照如下代码方式设置AVFrame的pts
      */
      audio_en_frame->pts = audio_pts;
      audio_en_frame->pts = av_rescale_q(audio_pts, (AVRational){1,audio_en_frame->sample_rate}, audio_en_ctx->time_base);
      audio_pts += frame_size;
      doEncodeAudio(audio_en_frame);
    }
  }
}

void Transcode::doEncodeAudio(AVFrame *frame)
{
  int ret = 0;
  if ((ret = avcodec_send_frame(audio_en_ctx, frame)) < 0) {
    LOG(ERROR) << "audio avcodec_send_frame fail: " << av_err2str(ret);
    return;
  }


  while (true) {
    AVPacket *pkt = av_packet_alloc();
    if ((ret = avcodec_receive_packet(audio_en_ctx, pkt)) < 0) {
      break;
    }

    AVStream *stream = outFmtCtx->streams[audio_ou_stream_index_];
    av_packet_rescale_ts(pkt, audio_en_ctx->time_base, stream->time_base);
    pkt->stream_index = audio_ou_stream_index_;

    doWrite(pkt, false);
  }
}

bool Transcode::doWrite(AVPacket *packet,bool isVideo)
{
  if (packet == nullptr) {
    LOG(INFO) << "flush";
  }

  AVPacket *a_pkt = nullptr;
  AVPacket *v_pkt = nullptr;
  AVPacket *w_pkt = nullptr;

  if (packet) {
    if (isVideo) {
      v_pkt = packet;
    } else {
      a_pkt = packet;
    }

    // 为了精确的比较时间，先缓存一点点数据
    if (last_video_pts == 0 && isVideo && videoCache.size() == 0) {
      videoCache.push_back(packet);
      last_video_pts = packet->pts;
      return true;
    }

    if (last_audio_pts == 0 && !isVideo && audioCache.size() == 0) {
      audioCache.push_back(packet);
      last_audio_pts = packet->pts;
      return true;
    }
  }

  if (videoCache.size() > 0) {    // 里面有缓存了数据
    v_pkt = videoCache.front();
    last_video_pts = v_pkt->pts;
    if (isVideo && packet) {
      videoCache.push_back(packet);
    }
  }

  if (audioCache.size() > 0) {    // 里面有缓存了数据
    a_pkt = audioCache.front();
    last_audio_pts = a_pkt->pts;
    if (!isVideo && packet) {
      audioCache.push_back(packet);
    }
  }

  AVStream *a_stream = outFmtCtx->streams[audio_ou_stream_index_];
  AVStream *v_stream = outFmtCtx->streams[video_ou_stream_index_];
  if (a_pkt && v_pkt) {   // 两个都有 则进行时间的比较
    if (av_compare_ts(last_audio_pts,a_stream->time_base,last_video_pts,v_stream->time_base) <= 0) { // 视频在后
      w_pkt = a_pkt;
      eraseApkt();
    } else {
      w_pkt = v_pkt;
      eraseVpkt();
    }
  } else if (a_pkt) {
    w_pkt = a_pkt;
    eraseApkt();
  } else if (v_pkt) {
    w_pkt = v_pkt;
    eraseVpkt();
  }

 if (av_interleaved_write_frame(outFmtCtx, w_pkt) < 0) {
    LOG(ERROR) <<"Error muxing packet";
    return false;
  }

  av_packet_free(&w_pkt);
  return true;
}

void Transcode::eraseVpkt() {
  if (videoCache.size() > 0) {
    std::vector<AVPacket*>::iterator begin = videoCache.begin();
    videoCache.erase(begin);
  }
}

void Transcode::eraseApkt() {
  if (audioCache.size() > 0) {
    std::vector<AVPacket*>::iterator begin = audioCache.begin();
    audioCache.erase(begin);
  }
}

AVFrame* Transcode::get_audio_frame(enum AVSampleFormat smpfmt,int64_t ch_layout,int sample_rate,int nb_samples)
{
  AVFrame * audio_en_frame = av_frame_alloc();
  // 根据采样格式，采样率，声道类型以及采样数分配一个AVFrame
  audio_en_frame->sample_rate = sample_rate;
  audio_en_frame->format = smpfmt;
  audio_en_frame->channel_layout = ch_layout;
  audio_en_frame->nb_samples = nb_samples;
  int ret = 0;
  if ((ret = av_frame_get_buffer(audio_en_frame, 0)) < 0) {
    LOG(ERROR) << "audio get frame buffer fail: " << ret;
    return nullptr;
  }

  if ((ret =  av_frame_make_writable(audio_en_frame)) < 0) {
    LOG(ERROR) << "audio av_frame_make_writable fail: " << ret;
    return nullptr;
  }

  return audio_en_frame;
}

AVFrame* Transcode::get_video_frame(enum AVPixelFormat pixfmt,int width,int height)
{
  AVFrame *video_en_frame = av_frame_alloc();
  video_en_frame->format = pixfmt;
  video_en_frame->width = width;
  video_en_frame->height = height;
  int ret = 0;
  if ((ret = av_frame_get_buffer(video_en_frame, 0)) < 0) {
    LOG(ERROR) << "video get frame buffer fail: " << ret;
    return nullptr;
  }

  if ((ret =  av_frame_make_writable(video_en_frame)) < 0) {
    LOG(ERROR) << "video av_frame_make_writable fail: " << ret;
    return nullptr;
  }

  return video_en_frame;
}

void Transcode::releaseSources()
{
  if (inFmtCtx) {
    avformat_close_input(&inFmtCtx);
    inFmtCtx = nullptr;
  }

  if (outFmtCtx) {
    avformat_free_context(outFmtCtx);
    outFmtCtx = nullptr;
  }

  if (video_en_ctx) {
    avcodec_free_context(&video_en_ctx);
    video_en_ctx = nullptr;
  }

  if (audio_en_ctx) {
    avcodec_free_context(&audio_en_ctx);
    audio_en_ctx = nullptr;
  }

  if (video_de_ctx) {
    avcodec_free_context(&video_de_ctx);
    video_de_ctx = nullptr;
  }

  if (audio_de_ctx) {
    avcodec_free_context(&audio_de_ctx);
    audio_de_ctx = nullptr;
  }

  if (video_de_frame) {
    av_frame_free(&video_de_frame);
    video_de_frame = nullptr;
  }

  if (audio_de_frame) {
    av_frame_free(&audio_de_frame);
  }

  if (video_en_frame) {
    av_frame_free(&video_en_frame);
    video_en_frame = nullptr;
  }

  if (audio_en_frame) {
    av_frame_free(&audio_en_frame);
    audio_en_frame = nullptr;
  }
}