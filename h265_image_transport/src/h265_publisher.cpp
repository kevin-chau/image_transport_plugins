// Copyright (c) 2023, Open Source Robotics Foundation, Inc.
// All rights reserved.
//
// Software License Agreement (BSD License 2.0)
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//
//  * Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
//  * Redistributions in binary form must reproduce the above
//    copyright notice, this list of conditions and the following
//    disclaimer in the documentation and/or other materials provided
//    with the distribution.
//  * Neither the name of the copyright holder nor the names of its
//    contributors may be used to endorse or promote products derived
//    from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
// FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
// COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
// INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
// BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
// LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
// ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#include "h265_image_transport/h265_publisher.hpp"

#include <iostream>
#include <vector>

#include <rclcpp/rclcpp.hpp>

extern "C" {
  #include <libavcodec/avcodec.h>
  #include <libavutil/opt.h>
  #include <libavutil/imgutils.h>
  #include <libswscale/swscale.h>
}

namespace h265_image_transport
{

// enum hParameters
// {
// };

// const struct ParameterDefinition kParameters[] =
// {
// };

H265Publisher::H265Publisher()
: logger_(rclcpp::get_logger("H265Publisher"))
{
  this->codec_ = avcodec_find_encoder(AV_CODEC_ID_H265);
  if (!this->codec_){
    std::cerr << "Codec with specified id not found" << std::endl;
    return;
  }

  // this->videoStream_ = avformat_new_stream(this->formatCtx,
  //     nullptr);

  this->context_ = avcodec_alloc_context3(this->codec_);
  if (!this->context_){
    std::cerr << "Can't allocate video codec context" << std::endl;
    return;
  }
}

H265Publisher::~H265Publisher()
{
}

std::string H265Publisher::getTransportName() const
{
  return "h265";
}

void H265Publisher::advertiseImpl(
  rclcpp::Node * node,
  const std::string & base_topic,
  rmw_qos_profile_t custom_qos,
  rclcpp::PublisherOptions options)
{
  node_ = node;
  typedef image_transport::SimplePublisherPlugin<sensor_msgs::msg::CompressedImage> Base;
  Base::advertiseImpl(node, base_topic, custom_qos, options);

  // Declare Parameters
  uint ns_len = node->get_effective_namespace().length();
  std::string param_base_name = base_topic.substr(ns_len);
  std::replace(param_base_name.begin(), param_base_name.end(), '/', '.');

  // for (const ParameterDefinition & pd : kParameters) {
  //   declareParameter(param_base_name, pd);
  // }
}

void H265Publisher::publish(
  const sensor_msgs::msg::Image & message,
  const PublishFn & publish_fn) const
{
  std::unique_lock<std::mutex> guard(this->mutex);

  /// Resolution must be a multiple of two
  if (this->context_->height == 0)
  {
    this->context_->height = message.height;
    this->context_->width = message.width;
    this->context_->pix_fmt = AV_PIX_FMT_YUV420P;
    this->context_->codec_type = AVMEDIA_TYPE_VIDEO;

    int fps = 30;
    /// Frames per second
    this->context_->time_base.num = 1;
    this->context_->time_base.den = fps;
    this->context_->framerate.num = fps;
    this->context_->framerate.den = 1;

    /// Key(intra) frame rate
    /// looks like option not works for H265 :(
    this->context_->gop_size = fps*2;
    this->context_->thread_count = 5;
    this->context_->max_b_frames = 0;

    this->context_->sw_pix_fmt = AV_PIX_FMT_YUV420P;

    /// P-frames, generated by referencing data from prev and future frames.
    /// [Compression up, CPU usage up]
    /// [use 3/gop]
    this->context_->max_b_frames = 3;

    /// Can be used by a P-frame(predictive, partial frame) to help define a future frame in a compressed video.
    /// [use 3–5 ref per P]
    this->context_->refs = 3;

    /// Compression efficiency (slower -> better quality + higher cpu%)
    /// [ultrafast, superfast, veryfast, faster, fast, medium, slow, slower, veryslow]
    /// Set this option to "ultrafast" is critical for realtime encoding
    av_opt_set(this->context_->priv_data, "preset", "ultrafast", 0);

    /// Compression rate (lower -> higher compression) compress to lower size, makes decoded image more noisy
    /// Range: [0; 51], sane range: [18; 26]. I used 35 as good compression/quality compromise. This option also critical for realtime encoding
    av_opt_set(this->context_->priv_data, "crf", "35", 0);

    /// Change settings based upon the specifics of input
    /// [psnr, ssim, grain, zerolatency, fastdecode, animation]
    /// This option is most critical for realtime encoding, because it removes delay between 1th input frame and 1th output packet.
    // av_opt_set(this->context_->priv_data, "tune", "zerolatency", 0);

    auto desc = av_pix_fmt_desc_get(AV_PIX_FMT_RGB24);
    if (!desc){
      std::cerr << "Can't get descriptor for pixel format" << std::endl;
      return;
    }
    auto bytesPerPixel = av_get_bits_per_pixel(desc) / 8;
    if(!(bytesPerPixel == 3 && !(av_get_bits_per_pixel(desc) % 8))) {
      std::cerr << "Unhandled bits per pixel, bad in pix fmt" << std::endl;
      return;
    }

    if(avcodec_open2(this->context_, this->codec_, nullptr) < 0){
      std::cerr << "Could not open codec" << std::endl;
      return;
    }
    std::cerr << "All good" << std::endl;

    this->avInFrame_ = av_frame_alloc();
    this->avInFrame_->width = message.width;
    this->avInFrame_->height = message.height;
    this->avInFrame_->format = AV_PIX_FMT_RGB24;

    if (av_frame_get_buffer(this->avInFrame_, 32) > 0)
    {
      std::cout << "Error av_frame_get_buffer avInFrame_" << std::endl;
    }

    av_image_fill_linesizes(this->inputLineSizes_,
                            AV_PIX_FMT_RGB24,
                            message.width);

    this->swsCtx_ = sws_getContext(
        message.width,
        message.height,
        AV_PIX_FMT_RGB24,
        this->context_->width,
        this->context_->height,
        // we misuse this field a bit, as docs say it is unused in encoders
        AV_PIX_FMT_RGB0,
        0, nullptr, nullptr, nullptr);

    this->avOutFrame_ = av_frame_alloc();
    // we misuse sw_pix_fmt a bit, as docs say it is unused in encoders
    this->avOutFrame_->format = this->context_->sw_pix_fmt;
    this->avOutFrame_->width = this->context_->width;
    this->avOutFrame_->height = this->context_->height;
    // av_image_alloc() could also allocate the image, but av_frame_get_buffer()
    // allocates a refcounted buffer, which is easier to manage
    if (av_frame_get_buffer(this->avOutFrame_, 32) > 0)
    {
      std::cout << "Error av_frame_get_buffer" << std::endl;
    }
  }

  const unsigned char *_frame = &message.data[0];
  // copy the unaligned input buffer to the 32-byte-aligned avInFrame
  av_image_copy(
      this->avInFrame_->data, this->avInFrame_->linesize,
      &_frame, this->inputLineSizes_,
      AV_PIX_FMT_YUV420P,
      message.width, message.height);

  sws_scale(this->swsCtx_,
      this->avInFrame_->data,
      this->avInFrame_->linesize,
      0, message.height,
      this->avOutFrame_->data,
      this->avOutFrame_->linesize);

  auto* frameToEncode = this->avOutFrame_;
  frameToEncode->pts = this->frameCount_++;

  // // compute frame number based on timestamp of current image
  // auto timeSinceStart = std::chrono::duration_cast<std::chrono::milliseconds>(
  //     _timestamp - this->dataPtr->timeStart);
  // double durationSec = timeSinceStart.count() / 1000.0;
  // uint64_t frameNumber = static_cast<uint64_t>(durationSec / period);

  AVPacket* avPacket = av_packet_alloc();

  avPacket->data = nullptr;
  avPacket->size = 0;

  auto ret = avcodec_send_frame(this->context_, frameToEncode);

  std::cout << "avavcodec_send_framePacket " << this->frameCount_ << std::endl;

  ret = -1;
  while( ret != 0)
  {
    ret = avcodec_send_frame(this->context_, frameToEncode);
    switch (ret){
      case 0:
        frameIdx = (frameIdx % this->context_->framerate.num) + 1;
        std::cout << "frameIdx " << frameIdx << " " << ret << std::endl;
        break;
      case AVERROR(EAGAIN):
        std::cout << "EAGAIN " << std::endl;
        continue;
      case AVERROR_EOF:
        std::cout << "AVERROR_EOF " << std::endl;
        continue;
      case AVERROR(EINVAL):
        std::cout << "EINVAL " << std::endl;
        continue;
      case AVERROR(ENOMEM):
        std::cout << "ENOMEM " << std::endl;
        continue;
      default:
        continue;
    }
  }


  // This loop will retrieve and write available packets
  while (ret >= 0)
  {
    ret = avcodec_receive_packet(this->context_, avPacket);

    if (avPacket->buf != nullptr)
    {
      std::cout << "avPacket " << avPacket->buf->size << std::endl;
    }


    // Potential performance improvement: Queue the packets and write in
    // a separate thread.
    if (ret >= 0) {

      // avPacket->stream_index = this->videoStream->index;

      // Scale timestamp appropriately.
      if (avPacket->pts != static_cast<int64_t>(AV_NOPTS_VALUE))
      {
        avPacket->pts = av_rescale_q(
          avPacket->pts,
          this->context_->time_base,
          this->context_->time_base);
      }

      if (avPacket->dts != static_cast<int64_t>(AV_NOPTS_VALUE))
      {
        avPacket->dts = av_rescale_q(
          avPacket->dts,
          this->context_->time_base,
          this->context_->time_base);
      }

      // Write frame to disk
      // auto ret2 = av_interleaved_write_frame(this->formatCtx, avPacket);

      sensor_msgs::msg::CompressedImage compressed;
      // buf_size + buf_data + pts + dts + data_size + data + flags + side_data_elems + side_data_elems_data + duration + pos;
      int size_data = 4 + avPacket->buf->size + 8 + 8 + avPacket->size + 4 + 4 + 4 + avPacket->side_data_elems + 8 + 8;

      for (int i = 0; i < avPacket->side_data_elems; ++i)
      {
        size_data += avPacket->side_data[i].size + sizeof(int) + sizeof(int);
      }

      int index_data_compressed = 0;
      compressed.data.resize(size_data);

      memcpy(&compressed.data[index_data_compressed], &avPacket->buf->size, sizeof(int));
      std::cout << "index_data_compressed buf->size " << index_data_compressed << std::endl;
      index_data_compressed += sizeof(int);

      memcpy(&compressed.data[index_data_compressed], &avPacket->buf->data[0], avPacket->buf->size);
      std::cout << "index_data_compressed buf->buff_data " << index_data_compressed << std::endl;
      index_data_compressed += avPacket->buf->size;

      memcpy(&compressed.data[index_data_compressed], &avPacket->size, sizeof(int));
      std::cout << "index_data_compressed " << index_data_compressed << std::endl;
      index_data_compressed += sizeof(int);

      memcpy(&compressed.data[index_data_compressed], &avPacket->data, avPacket->size);
      index_data_compressed += avPacket->size;

      memcpy(&compressed.data[index_data_compressed], &avPacket->pts, sizeof(int64_t));
      std::cout << "index_data_compressed pts " << index_data_compressed << std::endl;
      index_data_compressed += sizeof(int64_t);

      memcpy(&compressed.data[index_data_compressed], &avPacket->dts, sizeof(int64_t));
      std::cout << "index_data_compressed dts " << index_data_compressed << std::endl;
      index_data_compressed += sizeof(int64_t);

      memcpy(&compressed.data[index_data_compressed], &avPacket->stream_index, sizeof(int));
      index_data_compressed += sizeof(int);

      memcpy(&compressed.data[index_data_compressed], &avPacket->flags, sizeof(int));
      index_data_compressed += sizeof(int);

      std::cout << "avPacket->size " << avPacket->size << std::endl;
      std::cout << "avPacket->pts " << avPacket->pts << std::endl;
      std::cout << "avPacket->dts " << avPacket->dts << std::endl;
      std::cout << "avPacket->flags " << avPacket->flags << std::endl;
      std::cout << "avPacket->duration " << avPacket->duration << std::endl;
      std::cout << "avPacket->pos " << avPacket->pos << std::endl;
      std::cout << "avPacket->side_data_elems " << avPacket->side_data_elems << std::endl;

      memcpy(&compressed.data[index_data_compressed], &avPacket->side_data_elems, sizeof(int));
      index_data_compressed += sizeof(int);

      for (int i = 0; i < avPacket->side_data_elems; ++i)
      {
        memcpy(&compressed.data[index_data_compressed], &avPacket->side_data[i].size, sizeof(int));
        index_data_compressed += sizeof(int);
        std::cout << "avPacket->duration size: " << avPacket->side_data[i].size << std::endl;

        memcpy(&compressed.data[index_data_compressed], &avPacket->side_data[i].type, sizeof(int));
        index_data_compressed += sizeof(int);
        std::cout << "avPacket->duration type: " << avPacket->side_data[i].type << std::endl;

        memcpy(&compressed.data[index_data_compressed], &avPacket->side_data[i].data, avPacket->side_data[i].size);
        index_data_compressed += avPacket->side_data[i].size;
      }
      std::cout << "index_data_compressed: antes de duration " << index_data_compressed << std::endl;

      memcpy(&compressed.data[index_data_compressed], &avPacket->duration, sizeof(int64_t));
      index_data_compressed += sizeof(int64_t);

      memcpy(&compressed.data[index_data_compressed], &avPacket->pos, sizeof(int64_t));
      index_data_compressed += sizeof(int64_t);

      std::cout << "avPacket->duration " << avPacket->duration << std::endl;
      std::cout << "avPacket->pos " << avPacket->pos << std::endl;

      compressed.header = message.header;
      compressed.format = "h265";
      publish_fn(compressed);
    }
  }
  av_packet_unref(avPacket);
}

void H265Publisher::declareParameter(
  const std::string & base_name,
  const ParameterDefinition & definition)
{
  // transport scoped parameter (e.g. image_raw.compressed.format)
  const std::string transport_name = getTransportName();
  const std::string param_name = base_name + "." + transport_name + "." +
    definition.descriptor.name;
  parameters_.push_back(param_name);

  rclcpp::ParameterValue param_value;

  try {
    param_value = node_->declare_parameter(
      param_name, definition.defaultValue,
      definition.descriptor);
  } catch (const rclcpp::exceptions::ParameterAlreadyDeclaredException &) {
    RCLCPP_DEBUG(logger_, "%s was previously declared", definition.descriptor.name.c_str());
    param_value = node_->get_parameter(param_name).get_parameter_value();
  }
}
}  // namespace h265_image_transport
