//
// Copyright (c) 2013-2022 The SRS Authors
//
// SPDX-License-Identifier: MIT or MulanPSL-2.0
//

#include <srs_app_dash.hpp>

#include <srs_kernel_error.hpp>
#include <srs_app_source.hpp>
#include <srs_app_config.hpp>
#include <srs_protocol_rtmp_stack.hpp>
#include <srs_kernel_codec.hpp>
#include <srs_kernel_utility.hpp>
#include <srs_app_utility.hpp>
#include <srs_kernel_file.hpp>
#include <srs_core_autofree.hpp>
#include <srs_kernel_mp4.hpp>

#include <stdlib.h>
#include <sstream>
using namespace std;

static std::string format_float(const double& d, int width = 3)
{
    stringstream ss;
    ss.setf(std::ios::fixed);
    ss.precision(width);
    ss << d;

    return ss.str();
}

SrsInitMp4::SrsInitMp4()
{
    fw = new SrsFileWriter();
    init = new SrsMp4M2tsInitEncoder();
}

SrsInitMp4::~SrsInitMp4()
{
    srs_freep(init);
    srs_freep(fw);
}

srs_error_t SrsInitMp4::write(SrsFormat* format, bool video, int tid)
{
    srs_error_t err = srs_success;
    
    string path_tmp = tmppath();
    if ((err = fw->open(path_tmp)) != srs_success) {
        return srs_error_wrap(err, "Open init mp4 failed, path=%s", path_tmp.c_str());
    }
    
    if ((err = init->initialize(fw)) != srs_success) {
        return srs_error_wrap(err, "init");
    }
    
    if ((err = init->write(format, video, tid)) != srs_success) {
        return srs_error_wrap(err, "write init");
    }
    
    return err;
}

SrsFragmentedMp4::SrsFragmentedMp4()
{
    fw = new SrsFileWriter();
    enc = new SrsMp4M2tsSegmentEncoder();
}

SrsFragmentedMp4::~SrsFragmentedMp4()
{
    srs_freep(enc);
    srs_freep(fw);
}

srs_error_t SrsFragmentedMp4::initialize(SrsRequest* r, bool video, int64_t time, SrsMpdWriter* mpd, uint32_t tid)
{
    srs_error_t err = srs_success;
    
    string file_home;
    string file_name;
    int64_t sequence_number;
    if ((err = mpd->get_fragment(video, file_home, file_name, time, sequence_number)) != srs_success) {
        return srs_error_wrap(err, "get fragment");
    }

    string home = _srs_config->get_dash_path(r->vhost);
    set_path(home + "/" + file_home + "/" + file_name);
    
    if ((err = create_dir()) != srs_success) {
        return srs_error_wrap(err, "create dir");
    }
    
    string path_tmp = tmppath();
    if ((err = fw->open(path_tmp)) != srs_success) {
        return srs_error_wrap(err, "Open fmp4 failed, path=%s", path_tmp.c_str());
    }
    
    if ((err = enc->initialize(fw, (uint32_t)sequence_number, time, tid)) != srs_success) {
        return srs_error_wrap(err, "init encoder");
    }
    
    return err;
}

srs_error_t SrsFragmentedMp4::write(SrsSharedPtrMessage* shared_msg, SrsFormat* format)
{
    srs_error_t err = srs_success;
    
    if (shared_msg->is_audio()) {
        uint8_t* sample = (uint8_t*)format->raw;
        uint32_t nb_sample = (uint32_t)format->nb_raw;
        
        uint32_t dts = (uint32_t)shared_msg->timestamp;
        err = enc->write_sample(SrsMp4HandlerTypeSOUN, 0x00, dts, dts, sample, nb_sample);
    } else if (shared_msg->is_video()) {
        SrsVideoAvcFrameType frame_type = format->video->frame_type;
        uint32_t cts = (uint32_t)format->video->cts;
        
        uint32_t dts = (uint32_t)shared_msg->timestamp;
        uint32_t pts = dts + cts;
        
        uint8_t* sample = (uint8_t*)format->raw;
        uint32_t nb_sample = (uint32_t)format->nb_raw;
        err = enc->write_sample(SrsMp4HandlerTypeVIDE, frame_type, dts, pts, sample, nb_sample);
    } else {
        return err;
    }
    
    append(shared_msg->timestamp);
    
    return err;
}

srs_error_t SrsFragmentedMp4::reap(uint64_t& dts)
{
    srs_error_t err = srs_success;
    
    if ((err = enc->flush(dts)) != srs_success) {
        return srs_error_wrap(err, "Flush encoder failed");
    }
    
    srs_freep(fw);
    
    if ((err = rename()) != srs_success) {
        return srs_error_wrap(err, "rename");
    }
    
    return err;
}

SrsMpdWriter::SrsMpdWriter()
{
    req = NULL;
    timeshit = update_period = fragment = 0;
    last_update_mpd = 0;

    window_size_ = 0;
    availability_start_time_ = 0;
}

SrsMpdWriter::~SrsMpdWriter()
{
}

srs_error_t SrsMpdWriter::initialize(SrsRequest* r)
{
    req = r;
    return srs_success;
}

srs_error_t SrsMpdWriter::on_publish()
{
    SrsRequest* r = req;

    fragment = _srs_config->get_dash_fragment(r->vhost);
    update_period = _srs_config->get_dash_update_period(r->vhost);
    timeshit = _srs_config->get_dash_timeshift(r->vhost);
    home = _srs_config->get_dash_path(r->vhost);
    mpd_file = _srs_config->get_dash_mpd_file(r->vhost);

    string mpd_path = srs_path_build_stream(mpd_file, req->vhost, req->app, req->stream);
    fragment_home = srs_path_dirname(mpd_path) + "/" + req->stream;

    window_size_ = _srs_config->get_dash_window_size(r->vhost);
    availability_start_time_ = 0;

    srs_trace("DASH: Config fragment=%" PRId64 ", period=%" PRId64 ", windows size=%d", fragment, update_period, window_size_);

    return srs_success;
}

void SrsMpdWriter::on_unpublish()
{
}

srs_error_t SrsMpdWriter::write(SrsFormat* format, SrsFragmentWindow* afragments, SrsFragmentWindow* vfragments)
{
    srs_error_t err = srs_success;

    // TODO: FIXME: pure audio/video support.
    if (! window_size_ || afragments->size() < window_size_ || vfragments->size() < window_size_) {
        return err;
    }

    double last_duration = srsu2s(srs_max(vfragments->at(vfragments->size() - 1)->duration(), afragments->at(afragments->size() - 1)->duration()));
    
    // MPD is not expired?
    if (last_update_mpd != 0 && srs_get_system_time() - last_update_mpd < update_period) {
        return err;
    }
    last_update_mpd = srs_get_system_time();
    
    string mpd_path = srs_path_build_stream(mpd_file, req->vhost, req->app, req->stream);
    string full_path = home + "/" + mpd_path;
    string full_home = srs_path_dirname(full_path);
    
    fragment_home = srs_path_dirname(mpd_path) + "/" + req->stream;
    
    if ((err = srs_create_dir_recursively(full_home)) != srs_success) {
        return srs_error_wrap(err, "Create MPD home failed, home=%s", full_home.c_str());
    }
    
    stringstream ss;
    ss << "<?xml version=\"1.0\" encoding=\"utf-8\"?>" << endl
       << "<MPD profiles=\"urn:mpeg:dash:profile:isoff-live:2011,http://dashif.org/guidelines/dash-if-simple\" " << endl
       << "    ns1:schemaLocation=\"urn:mpeg:dash:schema:mpd:2011 DASH-MPD.xsd\" " << endl
       << "    xmlns=\"urn:mpeg:dash:schema:mpd:2011\" xmlns:ns1=\"http://www.w3.org/2001/XMLSchema-instance\" " << endl
       << "    type=\"dynamic\" " << endl
       << "    minimumUpdatePeriod=\"PT" << format_float(srsu2s(update_period)) << "S\" " << endl
       << "    timeShiftBufferDepth=\"PT" << format_float(last_duration * window_size_) << "S\" " << endl
       << "    availabilityStartTime=\"" << srs_time_to_utc_format_str(availability_start_time_) << "\" " << endl
       << "    publishTime=\"" << srs_get_system_time_utc_format_str() << "\" " << endl
       << "    minBufferTime=\"PT" << format_float(2 * last_duration) << "S\" >" << endl;

    ss << "    <BaseURL>" << req->stream << "/" << "</BaseURL>" << endl;
    
    ss << "    <Period start=\"PT0S\">" << endl;
    
    if (format->acodec && ! afragments->empty()) {
        ss << "        <AdaptationSet mimeType=\"audio/mp4\" segmentAlignment=\"true\" startWithSAP=\"1\">" << endl;
        ss << "            <Representation id=\"audio\" bandwidth=\"48000\" codecs=\"mp4a.40.2\">" << endl;
        ss << "                <SegmentTemplate initialization=\"$RepresentationID$-init.mp4\" "
                                            << "media=\"$RepresentationID$-$Time$.m4s\" "
                                            << "timescale=\"1000\">" << endl;
        ss << "                    <SegmentTimeline>" << endl;
        for (size_t i = afragments->size() - window_size_; i < afragments->size(); ++i) {
            ss << "                        <S t=\"" << srsu2ms(afragments->at(i)->get_start_dts()) << "\" "
                                          << "d=\"" << srsu2ms(afragments->at(i)->duration()) << "\" />"  << endl;
        }
        ss << "                    </SegmentTimeline>" << endl;
        ss << "                </SegmentTemplate>" << endl;
        ss << "            </Representation>" << endl;
        ss << "        </AdaptationSet>" << endl;
    }

    if (format->vcodec && ! vfragments->empty()) {
        int w = format->vcodec->width;
        int h = format->vcodec->height;
        ss << "        <AdaptationSet mimeType=\"video/mp4\" segmentAlignment=\"true\" startWithSAP=\"1\">" << endl;
        ss << "            <Representation id=\"video\" bandwidth=\"800000\" codecs=\"avc1.64001e\" " << "width=\"" << w << "\" height=\"" << h << "\">" << endl;
        ss << "                <SegmentTemplate initialization=\"$RepresentationID$-init.mp4\" "
                                            << "media=\"$RepresentationID$-$Time$.m4s\" "
                                            << "timescale=\"1000\">" << endl;
        ss << "                    <SegmentTimeline>" << endl;
        for (size_t i = vfragments->size() - window_size_; i < vfragments->size(); ++i) {
            ss << "                        <S t=\"" << srsu2ms(vfragments->at(i)->get_start_dts()) << "\" "
                                          << "d=\"" << srsu2ms(vfragments->at(i)->duration()) << "\" />"  << endl;
        }
        ss << "                    </SegmentTimeline>" << endl;
        ss << "                </SegmentTemplate>" << endl;
        ss << "            </Representation>" << endl;
        ss << "        </AdaptationSet>" << endl;
    }
    ss << "    </Period>" << endl;
    ss << "</MPD>" << endl;
    
    SrsFileWriter* fw = new SrsFileWriter();
    SrsAutoFree(SrsFileWriter, fw);
    
    string full_path_tmp = full_path + ".tmp";
    if ((err = fw->open(full_path_tmp)) != srs_success) {
        return srs_error_wrap(err, "Open MPD file=%s failed", full_path_tmp.c_str());
    }
    
    string content = ss.str();
    if ((err = fw->write((void*)content.data(), content.length(), NULL)) != srs_success) {
        return srs_error_wrap(err, "Write MPD file=%s failed", full_path.c_str());
    }
    
    if (::rename(full_path_tmp.c_str(), full_path.c_str()) < 0) {
        return srs_error_new(ERROR_DASH_WRITE_FAILED, "Rename %s to %s failed", full_path_tmp.c_str(), full_path.c_str());
    }
    
    srs_trace("DASH: Refresh MPD success, size=%dB, file=%s", content.length(), full_path.c_str());
    
    return err;
}

srs_error_t SrsMpdWriter::get_fragment(bool video, std::string& home, std::string& file_name, int64_t time, int64_t& sn)
{
    srs_error_t err = srs_success;
    
    home = fragment_home;

    // We name the segment as advanced N segments, because when we are generating segment at the current time,
    // the player may also request the current segment.
    srs_assert(fragment);

    sn++;
    if (video) {
        file_name = "video-" + srs_int2str(srsu2ms(time)) + ".m4s";
    } else {
        file_name = "audio-" + srs_int2str(srsu2ms(time)) + ".m4s";
    }
    
    return err;
}

void SrsMpdWriter::set_availability_start_time(srs_utime_t t)
{
    availability_start_time_ = t;
}

SrsDashController::SrsDashController()
{
    req = NULL;
    // trackid start from 1, because some player will check if track id is greater than 0
    video_track_id = 1;
    audio_track_id = 2;
    mpd = new SrsMpdWriter();
    vcurrent = acurrent = NULL;
    vfragments = new SrsFragmentWindow();
    afragments = new SrsFragmentWindow();
    audio_dts = video_dts = 0;
    first_dts_ = 0;
    fragment = 0;
}

SrsDashController::~SrsDashController()
{
    srs_freep(mpd);
    srs_freep(vcurrent);
    srs_freep(acurrent);
    srs_freep(vfragments);
    srs_freep(afragments);
}

srs_error_t SrsDashController::initialize(SrsRequest* r)
{
    srs_error_t err = srs_success;
    
    req = r;
    
    if ((err = mpd->initialize(r)) != srs_success) {
        return srs_error_wrap(err, "mpd");
    }
    
    return err;
}

srs_error_t SrsDashController::on_publish()
{
    srs_error_t err = srs_success;

    SrsRequest* r = req;

    fragment = _srs_config->get_dash_fragment(r->vhost);
    home = _srs_config->get_dash_path(r->vhost);

    if ((err = mpd->on_publish()) != srs_success) {
        return srs_error_wrap(err, "mpd");
    }

    srs_freep(vcurrent);
    srs_freep(acurrent);
    audio_dts = 0;
    video_dts = 0;
    first_dts_ = 0;

    return err;
}

void SrsDashController::on_unpublish()
{
    mpd->on_unpublish();

    srs_error_t err = srs_success;

    if ((err = vcurrent->reap(video_dts)) != srs_success) {
        srs_warn("reap video err %s", srs_error_desc(err).c_str());
        srs_freep(err);
    }
    srs_freep(vcurrent);

    if ((err = acurrent->reap(audio_dts)) != srs_success) {
        srs_warn("reap audio err %s", srs_error_desc(err).c_str());
        srs_freep(err);
    }
    srs_freep(acurrent);
}

srs_error_t SrsDashController::on_audio(SrsSharedPtrMessage* shared_audio, SrsFormat* format)
{
    srs_error_t err = srs_success;

    if (format->is_aac_sequence_header()) {
        return refresh_init_mp4(shared_audio, format);
    }

    audio_dts = shared_audio->timestamp;
    
    if (! acurrent) {
        acurrent = new SrsFragmentedMp4();
        
        if ((err = acurrent->initialize(req, false, audio_dts * SRS_UTIME_MILLISECONDS, mpd, audio_track_id)) != srs_success) {
            return srs_error_wrap(err, "Initialize the audio fragment failed");
        }
    }
    
    if (! first_dts_) {
        first_dts_ = audio_dts;
        mpd->set_availability_start_time(srs_get_system_time() - first_dts_ * SRS_UTIME_MILLISECONDS);
    }

    if (acurrent->duration() >= fragment) {
        // TODO: FIXME: bad function name(use to calc the real duration of this fragment)
        acurrent->append(shared_audio->timestamp);
        if ((err = acurrent->reap(audio_dts)) != srs_success) {
            return srs_error_wrap(err, "reap current");
        }
        
        afragments->append(acurrent);
        acurrent = new SrsFragmentedMp4();
        
        if ((err = acurrent->initialize(req, false, audio_dts * SRS_UTIME_MILLISECONDS, mpd, audio_track_id)) != srs_success) {
            return srs_error_wrap(err, "Initialize the audio fragment failed");
        }
    }
    
    if ((err = acurrent->write(shared_audio, format)) != srs_success) {
        return srs_error_wrap(err, "Write audio to fragment failed");
    }
    
    if ((err = refresh_mpd(format)) != srs_success) {
        return srs_error_wrap(err, "Refresh the MPD failed");
    }
    
    return err;
}

srs_error_t SrsDashController::on_video(SrsSharedPtrMessage* shared_video, SrsFormat* format)
{
    srs_error_t err = srs_success;

    if (format->is_avc_sequence_header()) {
        return refresh_init_mp4(shared_video, format);
    }

    video_dts = shared_video->timestamp;

    if (! vcurrent) {
        vcurrent = new SrsFragmentedMp4();
        
        if ((err = vcurrent->initialize(req, true, video_dts * SRS_UTIME_MILLISECONDS, mpd, video_track_id)) != srs_success) {
            return srs_error_wrap(err, "Initialize the video fragment failed");
        }
    }

    if (! first_dts_) {
        first_dts_ = video_dts;
        mpd->set_availability_start_time(srs_get_system_time() - first_dts_ * SRS_UTIME_MILLISECONDS);
    }
    
    bool reopen = format->video->frame_type == SrsVideoAvcFrameTypeKeyFrame && vcurrent->duration() >= fragment;
    if (reopen) {
        // TODO: FIXME: bad function name(use to calc the real duration of this fragment)
        vcurrent->append(shared_video->timestamp);
        if ((err = vcurrent->reap(video_dts)) != srs_success) {
            return srs_error_wrap(err, "reap current");
        }

        vfragments->append(vcurrent);
        vcurrent = new SrsFragmentedMp4();
        
        if ((err = vcurrent->initialize(req, true, video_dts * SRS_UTIME_MILLISECONDS, mpd, video_track_id)) != srs_success) {
            return srs_error_wrap(err, "Initialize the video fragment failed");
        }
    }
    
    if ((err = vcurrent->write(shared_video, format)) != srs_success) {
        return srs_error_wrap(err, "Write video to fragment failed");
    }
    
    if ((err = refresh_mpd(format)) != srs_success) {
        return srs_error_wrap(err, "Refresh the MPD failed");
    }
    
    return err;
}

srs_error_t SrsDashController::refresh_mpd(SrsFormat* format)
{
    srs_error_t err = srs_success;
    
    // TODO: FIXME: Support pure audio streaming.
    if (!format->acodec || !format->vcodec) {
        return err;
    }
    
    if ((err = mpd->write(format, afragments, vfragments)) != srs_success) {
        return srs_error_wrap(err, "write mpd");
    }
    
    return err;
}

srs_error_t SrsDashController::refresh_init_mp4(SrsSharedPtrMessage* msg, SrsFormat* format)
{
    srs_error_t err = srs_success;
    
    if (msg->size <= 0 || (msg->is_video() && !format->vcodec->is_avc_codec_ok())
        || (msg->is_audio() && !format->acodec->is_aac_codec_ok())) {
        srs_warn("DASH: Ignore empty sequence header.");
        return err;
    }
    
    string full_home = home + "/" + req->app + "/" + req->stream;
    if ((err = srs_create_dir_recursively(full_home)) != srs_success) {
        return srs_error_wrap(err, "Create media home failed, home=%s", full_home.c_str());
    }
    
    std::string path = full_home;
    if (msg->is_video()) {
        path += "/video-init.mp4";
    } else {
        path += "/audio-init.mp4";
    }
    
    SrsInitMp4* init_mp4 = new SrsInitMp4();
    SrsAutoFree(SrsInitMp4, init_mp4);
    
    init_mp4->set_path(path);
    
    int tid = msg->is_video()? video_track_id:audio_track_id;
    if ((err = init_mp4->write(format, msg->is_video(), tid)) != srs_success) {
        return srs_error_wrap(err, "write init");
    }
    
    if ((err = init_mp4->rename()) != srs_success) {
        return srs_error_wrap(err, "rename init");
    }
    
    srs_trace("DASH: Refresh media success, file=%s", path.c_str());
    
    return err;
}

SrsDash::SrsDash()
{
    hub = NULL;
    req = NULL;
    controller = new SrsDashController();
    
    enabled = false;
}

SrsDash::~SrsDash()
{
    srs_freep(controller);
}

srs_error_t SrsDash::initialize(SrsOriginHub* h, SrsRequest* r)
{
    srs_error_t err = srs_success;
    
    hub = h;
    req = r;
    
    if ((err = controller->initialize(req)) != srs_success) {
        return srs_error_wrap(err, "controller");
    }
    
    return err;
}

srs_error_t SrsDash::on_publish()
{
    srs_error_t err = srs_success;
    
    // Prevent duplicated publish.
    if (enabled) {
        return err;
    }
    
    if (!_srs_config->get_dash_enabled(req->vhost)) {
        return err;
    }
    enabled = true;

    if ((err = controller->on_publish()) != srs_success) {
        return srs_error_wrap(err, "controller");
    }
    
    return err;
}

srs_error_t SrsDash::on_audio(SrsSharedPtrMessage* shared_audio, SrsFormat* format)
{
    srs_error_t err = srs_success;
    
    if (!enabled) {
        return err;
    }

    if (!format->acodec) {
        return err;
    }

    if ((err = controller->on_audio(shared_audio, format)) != srs_success) {
        return srs_error_wrap(err, "Consume audio failed");
    }
    
    return err;
}

srs_error_t SrsDash::on_video(SrsSharedPtrMessage* shared_video, SrsFormat* format)
{
    srs_error_t err = srs_success;
    
    if (!enabled) {
        return err;
    }

    if (!format->vcodec) {
        return err;
    }
 
    if ((err = controller->on_video(shared_video, format)) != srs_success) {
        return srs_error_wrap(err, "Consume video failed");
    }
    
    return err;
}

void SrsDash::on_unpublish()
{
    // Prevent duplicated unpublish.
    if (!enabled) {
        return;
    }
    
    enabled = false;

    controller->on_unpublish();
}

