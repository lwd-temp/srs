//
// Copyright (c) 2013-2024 The SRS Authors
//
// SPDX-License-Identifier: MIT
//

#include <srs_app_srt_source.hpp>

#include <algorithm>
using namespace std;

#include <srs_kernel_flv.hpp>
#include <srs_kernel_utility.hpp>
#include <srs_kernel_buffer.hpp>
#include <srs_kernel_stream.hpp>
#include <srs_core_autofree.hpp>
#include <srs_protocol_raw_avc.hpp>
#include <srs_protocol_rtmp_stack.hpp>
#include <srs_app_source.hpp>
#include <srs_app_statistic.hpp>
#include <srs_app_pithy_print.hpp>

SrsSrtPacket::SrsSrtPacket()
{
    shared_buffer_ = NULL;
    actual_buffer_size_ = 0;
}

SrsSrtPacket::~SrsSrtPacket()
{
    srs_freep(shared_buffer_);
}

char* SrsSrtPacket::wrap(int size)
{
    // The buffer size is larger or equals to the size of packet.
    actual_buffer_size_ = size;

    // If the buffer is large enough, reuse it.
    if (shared_buffer_ && shared_buffer_->size >= size) {
        return shared_buffer_->payload;
    }

    // Create a large enough message, with under-layer buffer.
    srs_freep(shared_buffer_);
    shared_buffer_ = new SrsSharedPtrMessage();

    char* buf = new char[size];
    shared_buffer_->wrap(buf, size);

    return shared_buffer_->payload;
}

char* SrsSrtPacket::wrap(char* data, int size)
{
    char* buf = wrap(size);
    memcpy(buf, data, size);
    return buf;
}

char* SrsSrtPacket::wrap(SrsSharedPtrMessage* msg)
{
    // Generally, the wrap(msg) is used for RTMP to SRT, where the msg
    // is not generated by SRT.
    srs_freep(shared_buffer_);

    // Copy from the new message.
    shared_buffer_ = msg->copy();
    // If we wrap a message, the size of packet equals to the message size.
    actual_buffer_size_ = shared_buffer_->size;

    return msg->payload;
}

SrsSrtPacket* SrsSrtPacket::copy()
{
    SrsSrtPacket* cp = new SrsSrtPacket();

    cp->shared_buffer_ = shared_buffer_? shared_buffer_->copy2() : NULL;
    cp->actual_buffer_size_ = actual_buffer_size_;

    return cp;
}

char* SrsSrtPacket::data() 
{ 
    return shared_buffer_->payload; 
}

int SrsSrtPacket::size() 
{ 
    return shared_buffer_->size; 
}

SrsSrtSourceManager::SrsSrtSourceManager()
{
    lock = srs_mutex_new();
}

SrsSrtSourceManager::~SrsSrtSourceManager()
{
    srs_mutex_destroy(lock);
}

srs_error_t SrsSrtSourceManager::fetch_or_create(SrsRequest* r, SrsSharedPtr<SrsSrtSource>& pps)
{
    srs_error_t err = srs_success;

    // Use lock to protect coroutine switch.
    // @bug https://github.com/ossrs/srs/issues/1230
    SrsLocker(lock);

    string stream_url = r->get_stream_url();
    std::map< std::string, SrsSharedPtr<SrsSrtSource> >::iterator it = pool.find(stream_url);
    if (it != pool.end()) {
        SrsSharedPtr<SrsSrtSource> source = it->second;

        // we always update the request of resource,
        // for origin auth is on, the token in request maybe invalid,
        // and we only need to update the token of request, it's simple.
        source->update_auth(r);
        pps = source;

        return err;
    }

    SrsSharedPtr<SrsSrtSource> source(new SrsSrtSource());
    srs_trace("new srt source, stream_url=%s", stream_url.c_str());

    if ((err = source->initialize(r)) != srs_success) {
        return srs_error_wrap(err, "init source %s", r->get_stream_url().c_str());
    }

    pool[stream_url] = source;
    pps = source;

    return err;
}

void SrsSrtSourceManager::eliminate(SrsRequest* r)
{
    // Use lock to protect coroutine switch.
    // @bug https://github.com/ossrs/srs/issues/1230
    SrsLocker(lock);

    string stream_url = r->get_stream_url();
    std::map< std::string, SrsSharedPtr<SrsSrtSource> >::iterator it = pool.find(stream_url);
    if (it != pool.end()) {
        pool.erase(it);
    }
}

SrsSrtSourceManager* _srs_srt_sources = NULL;

SrsSrtConsumer::SrsSrtConsumer(SrsSharedPtr<SrsSrtSource> s)
{
    source_ = s;
    should_update_source_id = false;

    mw_wait = srs_cond_new();
    mw_min_msgs = 0;
    mw_waiting = false;
}

SrsSrtConsumer::~SrsSrtConsumer()
{
    source_->on_consumer_destroy(this);

    vector<SrsSrtPacket*>::iterator it;
    for (it = queue.begin(); it != queue.end(); ++it) {
        SrsSrtPacket* pkt = *it;
        srs_freep(pkt);
    }

    srs_cond_destroy(mw_wait);
}

void SrsSrtConsumer::update_source_id()
{
    should_update_source_id = true;
}

srs_error_t SrsSrtConsumer::enqueue(SrsSrtPacket* packet)
{
    srs_error_t err = srs_success;

    queue.push_back(packet);

    if (mw_waiting) {
        if ((int)queue.size() > mw_min_msgs) {
            srs_cond_signal(mw_wait);
            mw_waiting = false;
            return err;
        }
    }

    return err;
}

srs_error_t SrsSrtConsumer::dump_packet(SrsSrtPacket** ppkt)
{
    srs_error_t err = srs_success;

    if (should_update_source_id) {
        srs_trace("update source_id=%s/%s", source_->source_id().c_str(), source_->pre_source_id().c_str());
        should_update_source_id = false;
    }

    // TODO: FIXME: Refine performance by ring buffer.
    if (!queue.empty()) {
        *ppkt = queue.front();
        queue.erase(queue.begin());
    }

    return err;
}

void SrsSrtConsumer::wait(int nb_msgs, srs_utime_t timeout)
{
    mw_min_msgs = nb_msgs;

    // when duration ok, signal to flush.
    if ((int)queue.size() > mw_min_msgs) {
        return;
    }

    // the enqueue will notify this cond.
    mw_waiting = true;

    // use cond block wait for high performance mode.
    srs_cond_timedwait(mw_wait, timeout);
}

SrsSrtFrameBuilder::SrsSrtFrameBuilder(ISrsStreamBridge* bridge)
{
    ts_ctx_ = new SrsTsContext();

    sps_pps_change_ = false;
    sps_ = "";
    pps_ = "";

    req_ = NULL;
    bridge_ = bridge;

    video_streamid_ = 1;
    audio_streamid_ = 2;

    pp_audio_duration_ = new SrsAlonePithyPrint();
}

SrsSrtFrameBuilder::~SrsSrtFrameBuilder()
{
    srs_freep(ts_ctx_);
    srs_freep(req_);

    srs_freep(pp_audio_duration_);
}

srs_error_t SrsSrtFrameBuilder::on_publish()
{
    return srs_success;
}

srs_error_t SrsSrtFrameBuilder::on_packet(SrsSrtPacket *pkt)
{
    srs_error_t err = srs_success;

    char* buf = pkt->data();
    int nb_buf = pkt->size();

    // use stream to parse ts packet.
    int nb_packet = nb_buf / SRS_TS_PACKET_SIZE;
    for (int i = 0; i < nb_packet; i++) {
        char* p = buf + (i * SRS_TS_PACKET_SIZE);

        SrsBuffer* stream = new SrsBuffer(p, SRS_TS_PACKET_SIZE);
        SrsAutoFree(SrsBuffer, stream);

        // Process each ts packet. Note that the jitter of UDP may cause video glitch when packet loss or wrong seq. We
        // don't handle it because SRT will, see tlpktdrop at https://ossrs.net/lts/zh-cn/docs/v4/doc/srt-params
        if ((err = ts_ctx_->decode(stream, this)) != srs_success) {
            srs_warn("parse ts packet err=%s", srs_error_desc(err).c_str());
            srs_error_reset(err);
            continue;
        }
    }

    return err;
}

void SrsSrtFrameBuilder::on_unpublish()
{
}

srs_error_t SrsSrtFrameBuilder::initialize(SrsRequest* req)
{
    srs_error_t err = srs_success;

    // TODO: FIXME: check srt2rtmp enable in config.
    req_ = req->copy();

    return err;
}

srs_error_t SrsSrtFrameBuilder::on_ts_message(SrsTsMessage* msg)
{
    srs_error_t err = srs_success;
    
    // When the audio SID is private stream 1, we use common audio.
    // @see https://github.com/ossrs/srs/issues/740
    if (msg->channel->apply == SrsTsPidApplyAudio && msg->sid == SrsTsPESStreamIdPrivateStream1) {
        msg->sid = SrsTsPESStreamIdAudioCommon;
    }
    
    // when not audio/video, or not adts/annexb format, donot support.
    if (msg->stream_number() != 0) {
        return srs_error_new(ERROR_STREAM_CASTER_TS_ES, "ts: unsupported stream format, sid=%#x(%s-%d)",
            msg->sid, msg->is_audio()? "A":msg->is_video()? "V":"N", msg->stream_number());
    }
    
    // check supported codec
    if (msg->channel->stream != SrsTsStreamVideoH264 && msg->channel->stream != SrsTsStreamVideoHEVC && msg->channel->stream != SrsTsStreamAudioAAC) {
        return srs_error_new(ERROR_STREAM_CASTER_TS_CODEC, "ts: unsupported stream codec=%d", msg->channel->stream);
    }
    
    // parse the stream.
    SrsBuffer avs(msg->payload->bytes(), msg->payload->length());
    
    // publish audio or video.
    if (msg->channel->stream == SrsTsStreamVideoH264) {
        if ((err = on_ts_video_avc(msg, &avs)) != srs_success) {
            return srs_error_wrap(err, "ts: consume video");
        }
    }
    if (msg->channel->stream == SrsTsStreamAudioAAC) {
        if ((err = on_ts_audio(msg, &avs)) != srs_success) {
            return srs_error_wrap(err, "ts: consume audio");
        }
    }
    
    // TODO: FIXME: implements other codec?
#ifdef SRS_H265
    if (msg->channel->stream == SrsTsStreamVideoHEVC) {
        if ((err = on_ts_video_hevc(msg, &avs)) != srs_success) {
            return srs_error_wrap(err, "ts: consume hevc video");
        }
    }
#endif

    return err;
}

srs_error_t SrsSrtFrameBuilder::on_ts_video_avc(SrsTsMessage* msg, SrsBuffer* avs)
{
    srs_error_t err = srs_success;

    vector<pair<char*, int> > ipb_frames;

    SrsRawH264Stream* avc = new SrsRawH264Stream();
    SrsAutoFree(SrsRawH264Stream, avc);
    
    // send each frame.
    while (!avs->empty()) {
        char* frame = NULL;
        int frame_size = 0;
        if ((err = avc->annexb_demux(avs, &frame, &frame_size)) != srs_success) {
            return srs_error_wrap(err, "demux annexb");
        }

        if (frame == NULL || frame_size == 0) {
            continue;
        }
        
        // for sps
        if (avc->is_sps(frame, frame_size)) {
            std::string sps;
            if ((err = avc->sps_demux(frame, frame_size, sps)) != srs_success) {
                return srs_error_wrap(err, "demux sps");
            }
            
            if (! sps.empty() && sps_ != sps) {
                sps_pps_change_ = true;
            }

            sps_ = sps;
            continue;
        }
        
        // for pps
        if (avc->is_pps(frame, frame_size)) {
            std::string pps;
            if ((err = avc->pps_demux(frame, frame_size, pps)) != srs_success) {
                return srs_error_wrap(err, "demux pps");
            }
            
            if (! pps.empty() && pps_ != pps) {
                sps_pps_change_ = true;
            }

            pps_ = pps;
            continue;
        }

        ipb_frames.push_back(make_pair(frame, frame_size));
    }

    if ((err = check_sps_pps_change(msg)) != srs_success) {
        return srs_error_wrap(err, "check sps pps");
    }

    return on_h264_frame(msg, ipb_frames);
}

srs_error_t SrsSrtFrameBuilder::check_sps_pps_change(SrsTsMessage* msg)
{
    srs_error_t err = srs_success;

    if (! sps_pps_change_) {
        return err;
    }

    if (sps_.empty() || pps_.empty()) {
        return srs_error_new(ERROR_SRT_TO_RTMP_EMPTY_SPS_PPS, "sps or pps empty");
    }

    // sps/pps changed, generate new video sh frame and dispatch it.
    sps_pps_change_ = false;

    // ts tbn to flv tbn.
    uint32_t dts = (uint32_t)(msg->dts / 90);

    std::string sh;
    SrsRawH264Stream* avc = new SrsRawH264Stream();
    SrsAutoFree(SrsRawH264Stream, avc);

    if ((err = avc->mux_sequence_header(sps_, pps_, sh)) != srs_success) {
        return srs_error_wrap(err, "mux sequence header");
    }

    // h264 packet to flv packet.
    char* flv = NULL;
    int nb_flv = 0;
    if ((err = avc->mux_avc2flv(sh, SrsVideoAvcFrameTypeKeyFrame, SrsVideoAvcFrameTraitSequenceHeader, dts, dts, &flv, &nb_flv)) != srs_success) {
        return srs_error_wrap(err, "avc to flv");
    }

    SrsMessageHeader header;
    header.initialize_video(nb_flv, dts, video_streamid_);
    SrsCommonMessage rtmp;
    if ((err = rtmp.create(&header, flv, nb_flv)) != srs_success) {
        return srs_error_wrap(err, "create rtmp");
    }

    SrsSharedPtrMessage frame;
    if ((err = frame.create(&rtmp)) != srs_success) {
        return srs_error_wrap(err, "create frame");
    }

    if ((err = bridge_->on_frame(&frame)) != srs_success) {
        return srs_error_wrap(err, "srt to rtmp sps/pps");
    }

    return err;
}

srs_error_t SrsSrtFrameBuilder::on_h264_frame(SrsTsMessage* msg, vector<pair<char*, int> >& ipb_frames)
{
    srs_error_t err = srs_success;

    if (ipb_frames.empty()) {
        return srs_error_new(ERROR_SRT_CONN, "empty frame");
    }

    bool is_keyframe = false;

    // ts tbn to flv tbn.
    uint32_t dts = (uint32_t)(msg->dts / 90);
    uint32_t pts = (uint32_t)(msg->pts / 90);
    int32_t cts = pts - dts;

    int frame_size = 5; // 5bytes video tag header
    for (size_t i = 0; i != ipb_frames.size(); ++i) {
        // 4 bytes for nalu length.
        frame_size += 4 + ipb_frames[i].second;
        if (((SrsAvcNaluType)(ipb_frames[i].first[0] & 0x1f)) == SrsAvcNaluTypeIDR) {
            is_keyframe = true;
        }
    }

    SrsCommonMessage rtmp;
    rtmp.header.initialize_video(frame_size, dts, video_streamid_);
    rtmp.create_payload(frame_size);
    rtmp.size = frame_size;
    SrsBuffer payload(rtmp.payload, rtmp.size);
    // Write 5bytes video tag header.
    if (is_keyframe) {
        payload.write_1bytes(0x17); // type(4 bits): key frame; code(4bits): avc
    } else {
        payload.write_1bytes(0x27); // type(4 bits): inter frame; code(4bits): avc
    }
    payload.write_1bytes(0x01); // avc_type: nalu
    payload.write_3bytes(cts);  // composition time

    // Write video nalus.
    for (size_t i = 0; i != ipb_frames.size(); ++i) {
        char* nal = ipb_frames[i].first;
        int nal_size = ipb_frames[i].second;

        // write 4 bytes of nalu length.
        payload.write_4bytes(nal_size);
        // write nalu
        payload.write_bytes(nal, nal_size);
    }

    SrsSharedPtrMessage frame;
    if ((err = frame.create(&rtmp)) != srs_success) {
        return srs_error_wrap(err, "create frame");
    }

    if ((err = bridge_->on_frame(&frame)) != srs_success) {
        return srs_error_wrap(err ,"srt ts video to rtmp");
    }

    return err;
}

#ifdef SRS_H265
srs_error_t SrsSrtFrameBuilder::on_ts_video_hevc(SrsTsMessage *msg, SrsBuffer *avs)
{
    srs_error_t err = srs_success;

    vector<pair<char*, int> > ipb_frames;

    SrsRawHEVCStream *hevc = new SrsRawHEVCStream();
    SrsAutoFree(SrsRawHEVCStream, hevc);

    std::vector<std::string> hevc_pps;
    // send each frame.
    while (!avs->empty()) {
        char* frame = NULL;
        int frame_size = 0;
        if ((err = hevc->annexb_demux(avs, &frame, &frame_size)) != srs_success) {
            return srs_error_wrap(err, "demux hevc annexb");
        }

        if (frame == NULL || frame_size == 0) {
            continue;
        }

        // for vps
        if (hevc->is_vps(frame, frame_size)) {
            std::string vps;
            if ((err = hevc->vps_demux(frame, frame_size, vps)) != srs_success) {
                return srs_error_wrap(err, "demux vps");
            }

            if (!vps.empty() && hevc_vps_ != vps) {
                vps_sps_pps_change_ = true;
            }

            hevc_vps_ = vps;
            continue;
        }

        // for sps
        if (hevc->is_sps(frame, frame_size)) {
            std::string sps;
            if ((err = hevc->sps_demux(frame, frame_size, sps)) != srs_success) {
                return srs_error_wrap(err, "demux sps");
            }

            if (! sps.empty() && hevc_sps_ != sps) {
                vps_sps_pps_change_ = true;
            }

            hevc_sps_ = sps;
            continue;
        }

        // for pps
        if (hevc->is_pps(frame, frame_size)) {
            std::string pps;
            if ((err = hevc->pps_demux(frame, frame_size, pps)) != srs_success) {
                return srs_error_wrap(err, "demux pps");
            }

            if (!pps.empty()) {
                vps_sps_pps_change_ = true;
            }

            hevc_pps.push_back(pps);
            continue;
        }

        ipb_frames.push_back(make_pair(frame, frame_size));
    }

    if (!hevc_pps.empty()) {
        hevc_pps_ = hevc_pps;
    }

    if ((err = check_vps_sps_pps_change(msg)) != srs_success) {
        return srs_error_wrap(err, "check vps sps pps");
    }

    return on_hevc_frame(msg, ipb_frames);
}

srs_error_t SrsSrtFrameBuilder::check_vps_sps_pps_change(SrsTsMessage* msg)
{
    srs_error_t err = srs_success;

    if (!vps_sps_pps_change_) {
        return err;
    }

    if (hevc_vps_.empty() || hevc_sps_.empty() || hevc_pps_.empty()) {
        return err;
    }

    // vps/sps/pps changed, generate new video sh frame and dispatch it.
    vps_sps_pps_change_ = false;

    // ts tbn to flv tbn.
    uint32_t dts = (uint32_t)(msg->dts / 90);

    std::string sh;
    SrsRawHEVCStream* hevc = new SrsRawHEVCStream();
    SrsAutoFree(SrsRawHEVCStream, hevc);

    if ((err = hevc->mux_sequence_header(hevc_vps_, hevc_sps_, hevc_pps_, sh)) != srs_success) {
        return srs_error_wrap(err, "mux sequence header");
    }

    // h265 packet to flv packet.
    char* flv = NULL;
    int nb_flv = 0;
    if ((err = hevc->mux_avc2flv(sh, SrsVideoAvcFrameTypeKeyFrame, SrsVideoAvcFrameTraitSequenceHeader, dts, dts, &flv, &nb_flv)) != srs_success) {
        return srs_error_wrap(err, "avc to flv");
    }

    SrsMessageHeader header;
    header.initialize_video(nb_flv, dts, video_streamid_);
    SrsCommonMessage rtmp;
    if ((err = rtmp.create(&header, flv, nb_flv)) != srs_success) {
        return srs_error_wrap(err, "create rtmp");
    }

    SrsSharedPtrMessage frame;
    if ((err = frame.create(&rtmp)) != srs_success) {
        return srs_error_wrap(err, "create frame");
    }

    if ((err = bridge_->on_frame(&frame)) != srs_success) {
        return srs_error_wrap(err, "srt to rtmp vps/sps/pps");
    }

    return err;
}

srs_error_t SrsSrtFrameBuilder::on_hevc_frame(SrsTsMessage* msg, vector<pair<char*, int> >& ipb_frames)
{
    srs_error_t err = srs_success;

    if (ipb_frames.empty()) {
        return err;
    }

    // ts tbn to flv tbn.
    uint32_t dts = (uint32_t)(msg->dts / 90);
    uint32_t pts = (uint32_t)(msg->pts / 90);
    int32_t cts = pts - dts;

    // for IDR frame, the frame is keyframe.
    SrsVideoAvcFrameType frame_type = SrsVideoAvcFrameTypeInterFrame;

    // 5bytes video tag header
    int frame_size = 5;
    for (size_t i = 0; i != ipb_frames.size(); ++i) {
        // 4 bytes for nalu length.
        frame_size += 4 + ipb_frames[i].second;
        SrsHevcNaluType nalu_type = SrsHevcNaluTypeParse(ipb_frames[i].first[0]);
        if ((nalu_type >= SrsHevcNaluType_CODED_SLICE_BLA) && (nalu_type <= SrsHevcNaluType_RESERVED_23)) {
            frame_type = SrsVideoAvcFrameTypeKeyFrame;
        }
    }

    SrsCommonMessage rtmp;
    rtmp.header.initialize_video(frame_size, dts, video_streamid_);
    rtmp.create_payload(frame_size);
    rtmp.size = frame_size;
    SrsBuffer payload(rtmp.payload, rtmp.size);

    // Write 5bytes video tag header.

    // @see: E.4.3 Video Tags, video_file_format_spec_v10_1.pdf, page 78
    // Frame Type, Type of video frame.
    // CodecID, Codec Identifier.
    // set the rtmp header
    payload.write_1bytes((frame_type << 4) | SrsVideoCodecIdHEVC);
    // hevc_type: nalu
    payload.write_1bytes(0x01);
    // composition time
    payload.write_3bytes(cts);

    // Write video nalus.
    for (size_t i = 0; i != ipb_frames.size(); ++i) {
        char* nal = ipb_frames[i].first;
        int nal_size = ipb_frames[i].second;

        // write 4 bytes of nalu length.
        payload.write_4bytes(nal_size);
        // write nalu
        payload.write_bytes(nal, nal_size);
    }

    SrsSharedPtrMessage frame;
    if ((err = frame.create(&rtmp)) != srs_success) {
        return srs_error_wrap(err, "create frame");
    }

    if ((err = bridge_->on_frame(&frame)) != srs_success) {
        return srs_error_wrap(err ,"srt ts hevc video to rtmp");
    }

    return err;
}
#endif

srs_error_t SrsSrtFrameBuilder::on_ts_audio(SrsTsMessage* msg, SrsBuffer* avs)
{
    srs_error_t err = srs_success;
    
    SrsRawAacStream* aac = new SrsRawAacStream();
    SrsAutoFree(SrsRawAacStream, aac);

    // ts tbn to flv tbn.
    uint32_t pts = (uint32_t)(msg->pts / 90);

    int frame_idx = 0;
    int duration_ms = 0;
    
    // send each frame.
    while (!avs->empty()) {
        char* frame = NULL;
        int frame_size = 0;
        SrsRawAacStreamCodec codec;
        if ((err = aac->adts_demux(avs, &frame, &frame_size, codec)) != srs_success) {
            return srs_error_wrap(err, "demux adts");
        }
        
        // ignore invalid frame,
        //  * atleast 1bytes for aac to decode the data.
        if (frame_size <= 0) {
            continue;
        }
        
        std::string sh;
        if ((err = aac->mux_sequence_header(&codec, sh)) != srs_success) {
            return srs_error_wrap(err, "mux sequence header");
        }

        if (! sh.empty() && sh != audio_sh_) {
            audio_sh_ = sh;
            audio_sh_change_ = true;
        }

        // May have more than one aac frame in PES packet, and shared same timestamp,
        // so we must calculate each aac frame's timestamp.
        int sample_rate = 44100;
        switch (codec.sound_rate) {
            case SrsAudioSampleRate5512: sample_rate = 5512; break;
            case SrsAudioSampleRate11025: sample_rate = 11025; break;
            case SrsAudioSampleRate22050: sample_rate = 22050; break;
            case SrsAudioSampleRate44100: 
            default: sample_rate = 44100; break;
        }
        uint32_t frame_pts = (double)pts + (frame_idx * (1024.0 * 1000.0 / sample_rate));
        duration_ms += 1024.0 * 1000.0 / sample_rate;
        ++frame_idx;

        if ((err = check_audio_sh_change(msg, frame_pts)) != srs_success) {
            return srs_error_wrap(err, "audio sh");
        }

        if ((err = on_aac_frame(msg, frame_pts, frame, frame_size)) != srs_success) {
            return srs_error_wrap(err, "audio frame");
        }
    }

    pp_audio_duration_->elapse();

    if ((duration_ms >= 200) && pp_audio_duration_->can_print()) {
        // MPEG-TS always merge multi audio frame into one pes packet, may cause high latency and AV synchronization errors
        // @see https://github.com/ossrs/srs/issues/3164
        srs_warn("srt to rtmp, audio duration=%dms too large, audio frames=%d, may cause high latency and AV synchronization errors, "
            "read https://ossrs.io/lts/en-us/docs/v5/doc/srt-codec#ffmpeg-push-srt-stream", duration_ms, frame_idx);
    }
    
    return err;
}

srs_error_t SrsSrtFrameBuilder::check_audio_sh_change(SrsTsMessage* msg, uint32_t pts)
{
    srs_error_t err = srs_success;
    
    if (! audio_sh_change_) {
        return err;
    }

    // audio specific config changed, generate new audio sh and dispatch it.
    audio_sh_change_ = false;

    int rtmp_len = audio_sh_.size() + 2;

    SrsCommonMessage rtmp;
    rtmp.header.initialize_audio(rtmp_len, pts, audio_streamid_);
    rtmp.create_payload(rtmp_len);
    rtmp.size = rtmp_len;

    SrsBuffer stream(rtmp.payload, rtmp_len);
    uint8_t aac_flag = (SrsAudioCodecIdAAC << 4) | (SrsAudioSampleRate44100 << 2) | (SrsAudioSampleBits16bit << 1) | SrsAudioChannelsStereo;
    stream.write_1bytes(aac_flag);
    stream.write_1bytes(0);
    stream.write_bytes((char*)audio_sh_.data(), audio_sh_.size());

    SrsSharedPtrMessage frame;
    if ((err = frame.create(&rtmp)) != srs_success) {
        return srs_error_wrap(err, "create frame");
    }
    
    if ((err = bridge_->on_frame(&frame)) != srs_success) {
        return srs_error_wrap(err, "srt to rtmp audio sh");
    }

    return err;
}

srs_error_t SrsSrtFrameBuilder::on_aac_frame(SrsTsMessage* msg, uint32_t pts, char* data, int data_size)
{
    srs_error_t err = srs_success;
    
    int rtmp_len = data_size + 2/* 2 bytes of flv audio tag header*/;

    SrsCommonMessage rtmp;
    rtmp.header.initialize_audio(rtmp_len, pts, audio_streamid_);
    rtmp.create_payload(rtmp_len);
    rtmp.size = rtmp_len;

    SrsBuffer stream(rtmp.payload, rtmp_len);
    uint8_t aac_flag = (SrsAudioCodecIdAAC << 4) | (SrsAudioSampleRate44100 << 2) | (SrsAudioSampleBits16bit << 1) | SrsAudioChannelsStereo;
    // Write 2bytes audio tag header.
    stream.write_1bytes(aac_flag);
    stream.write_1bytes(1);
    // Write audio frame.
    stream.write_bytes(data, data_size);

    SrsSharedPtrMessage frame;
    if ((err = frame.create(&rtmp)) != srs_success) {
        return srs_error_wrap(err, "create frame");
    }
    
    if ((err = bridge_->on_frame(&frame)) != srs_success) {
        return srs_error_wrap(err, "srt to rtmp audio sh");
    }

    return err;
}

SrsSrtSource::SrsSrtSource()
{
    req = NULL;
    can_publish_ = true;
    frame_builder_ = NULL;
    bridge_ = NULL;
}

SrsSrtSource::~SrsSrtSource()
{
    // never free the consumers,
    // for all consumers are auto free.
    consumers.clear();

    srs_freep(frame_builder_);
    srs_freep(bridge_);
    srs_freep(req);
}

srs_error_t SrsSrtSource::initialize(SrsRequest* r)
{
    srs_error_t err = srs_success;

    req = r->copy();

	return err;
}

srs_error_t SrsSrtSource::on_source_id_changed(SrsContextId id)
{
    srs_error_t err = srs_success;
    
    if (!_source_id.compare(id)) {
        return err;
    }

    if (_pre_source_id.empty()) {
        _pre_source_id = id;
    }
    _source_id = id;
    
    // notice all consumer
    std::vector<SrsSrtConsumer*>::iterator it;
    for (it = consumers.begin(); it != consumers.end(); ++it) {
        SrsSrtConsumer* consumer = *it;
        consumer->update_source_id();
    }
    
    return err;
}

SrsContextId SrsSrtSource::source_id()
{
    return _source_id;
}

SrsContextId SrsSrtSource::pre_source_id()
{
    return _pre_source_id;
}

void SrsSrtSource::update_auth(SrsRequest* r)
{
    req->update_auth(r);
}

void SrsSrtSource::set_bridge(ISrsStreamBridge* bridge)
{
    srs_freep(bridge_);
    bridge_ = bridge;

    srs_freep(frame_builder_);
    frame_builder_ = new SrsSrtFrameBuilder(bridge);
}

srs_error_t SrsSrtSource::create_consumer(SrsSharedPtr<SrsSrtSource> source, SrsSrtConsumer*& consumer)
{
    srs_error_t err = srs_success;

    consumer = new SrsSrtConsumer(source);
    consumers.push_back(consumer);

    return err;
}

srs_error_t SrsSrtSource::consumer_dumps(SrsSrtConsumer* consumer)
{
    srs_error_t err = srs_success;

    // print status.
    srs_trace("create ts consumer, no gop cache");

    return err;
}

void SrsSrtSource::on_consumer_destroy(SrsSrtConsumer* consumer)
{
    std::vector<SrsSrtConsumer*>::iterator it;
    it = std::find(consumers.begin(), consumers.end(), consumer);
    if (it != consumers.end()) {
        it = consumers.erase(it);
    }

    // Destroy and cleanup source when no publishers and consumers.
    if (can_publish_ && consumers.empty()) {
        _srs_srt_sources->eliminate(req);
    }
}

bool SrsSrtSource::can_publish()
{
    return can_publish_;
}

srs_error_t SrsSrtSource::on_publish()
{
    srs_error_t err = srs_success;

    can_publish_ = false;

    if ((err = on_source_id_changed(_srs_context->get_id())) != srs_success) {
        return srs_error_wrap(err, "source id change");
    }

    if (bridge_) {
        if ((err = frame_builder_->initialize(req)) != srs_success) {
            return srs_error_wrap(err, "frame builder initialize");
        }

        if ((err = frame_builder_->on_publish()) != srs_success) {
            return srs_error_wrap(err, "frame builder on publish");
        }

        if ((err = bridge_->on_publish()) != srs_success) {
            return srs_error_wrap(err, "bridge on publish");
        }
    }

    SrsStatistic* stat = SrsStatistic::instance();
    stat->on_stream_publish(req, _source_id.c_str());

    return err;
}

void SrsSrtSource::on_unpublish()
{
    // ignore when already unpublished.
    if (can_publish_) {
        return;
    }

    can_publish_ = true;

    if (bridge_) {
        frame_builder_->on_unpublish();
        srs_freep(frame_builder_);

        bridge_->on_unpublish();
        srs_freep(bridge_);
    }

    // Destroy and cleanup source when no publishers and consumers.
    if (can_publish_ && consumers.empty()) {
        _srs_srt_sources->eliminate(req);
    }
}

srs_error_t SrsSrtSource::on_packet(SrsSrtPacket* packet)
{
    srs_error_t err = srs_success;

    for (int i = 0; i < (int)consumers.size(); i++) {
        SrsSrtConsumer* consumer = consumers.at(i);
        if ((err = consumer->enqueue(packet->copy())) != srs_success) {
            return srs_error_wrap(err, "consume ts packet");
        }
    }

    if (frame_builder_ && (err = frame_builder_->on_packet(packet)) != srs_success) {
        return srs_error_wrap(err, "bridge consume message");
    }

    return err;
}

