//
// Copyright (c) 2013-2021 Runner365
//
// SPDX-License-Identifier: MIT
//

#include "srt_conn.hpp"
#include "time_help.h"
#include "stringex.hpp"
#include "srt_log.hpp"
#include <vector>

#include <srs_protocol_utility.hpp>
#include <srs_app_config.hpp>

bool is_streamid_valid(const std::string& streamid) {
    if (streamid.empty()) {
        return false;
    }

    size_t pos = streamid.find(" ");
    if (pos != streamid.npos) {
        return false;
    }

    int mode;
    std::string subpath;
    std::string vhost;

    // Parse the stream info from streamid, see https://github.com/ossrs/srs/issues/2893
    bool ret = get_streamid_info(streamid, mode, vhost, subpath);
    if (!ret) {
        return false;
    }

    std::vector<std::string> info_vec;
    string_split(subpath, "/", info_vec);

    if (info_vec.size() != 2) {
        srt_log_warn("path format must be appname/stream?key=value...");
        return false;
    }

    for (auto item : info_vec) {
        if (item.empty()) {
            return false;
        }
        pos = item.find(" ");
        if (pos != item.npos) {
            return false;
        }
    }
    return true;
}

bool get_key_value(const std::string& info, std::string& key, std::string& value) {
    size_t pos = info.find("=");

    if (pos == info.npos) {
        return false;
    }

    key = info.substr(0, pos);
    value = info.substr(pos+1);

    if (key.empty() || value.empty()) {
        return false;
    }
    return true;
}

//eg. streamid=#!::h:live/livestream,m:publish
bool get_streamid_info(const std::string& streamid, int& mode, std::string& vhost, std::string& url_subpath) {

    mode = PULL_SRT_MODE;

    size_t pos = streamid.find("#!::");
    if (pos != 0) {
        pos = streamid.find("/");
        if (pos == streamid.npos) {
            url_subpath = _srs_config->get_default_app_name() + "/" + streamid;
            return true;
        }
        url_subpath = streamid;
        return true;
    }

    //SRT url supports multiple QueryStrings, which are passed to RTMP to realize authentication and other capabilities
    //@see https://github.com/ossrs/srs/issues/2893
    std::string params;
    std::string real_streamid;
    real_streamid = streamid.substr(4);

    std::map<std::string, std::string> query;
    srs_parse_query_string(real_streamid, query);
    for (std::map<std::string, std::string>::iterator it = query.begin(); it != query.end(); ++it) {
        if (it->first == "h") {
            params.append("vhost=");
            params.append(it->second);
            params.append("&");
            vhost = it->second;
        } else if (it->first == "r") {
            url_subpath = it->second;
        } else if (it->first == "m") {
            std::string mode_str = it->second; // support m=publish or m=request
            std::transform(it->second.begin(), it->second.end(), mode_str.begin(), ::tolower);
            if (mode_str == "publish") {
                mode = PUSH_SRT_MODE;
            }  else if (mode_str == "request") {
                mode = PULL_SRT_MODE;
            }  else {
                srt_log_warn("unknown mode_str:%s", mode_str.c_str());
                return false;
            }
        } else {
            params.append(it->first);
            params.append("=");
            params.append(it->second);
            params.append("&");
        }
    }

    if (url_subpath.empty())
        return false;

    if (!params.empty()) {
        url_subpath.append("?");
        url_subpath.append(params);
        url_subpath.pop_back(); // remove last '&'
    }

    return true;
}

srt_conn::srt_conn(SRTSOCKET conn_fd, const std::string& streamid):_conn_fd(conn_fd),
    _streamid(streamid),
    write_fail_cnt_(0) {
    get_streamid_info(streamid, _mode, _vhost, _url_subpath);
    
    _update_timestamp = now_ms();

    if (_vhost.empty()) {
        _vhost = "__default_host__";
    }

    srt_log_trace("srt connect construct streamid:%s, mode:%d, subpath:%s, vhost:%s", 
        streamid.c_str(), _mode, _url_subpath.c_str(), _vhost.c_str());
}

srt_conn::~srt_conn() {
    close();
}

std::string srt_conn::get_vhost() {
    return _vhost;
}

void srt_conn::update_timestamp(long long now_ts) {
    _update_timestamp = now_ts;
}

long long srt_conn::get_last_ts() {
    return _update_timestamp;
}

void srt_conn::close() {
    if (_conn_fd == SRT_INVALID_SOCK) {
        return;
    }
    srt_close(_conn_fd);
    _conn_fd = SRT_INVALID_SOCK;
}

SRTSOCKET srt_conn::get_conn() {
    return _conn_fd;
}
int srt_conn::get_mode() {
    return _mode;
}

std::string srt_conn::get_streamid() {
    return _streamid;
}

std::string srt_conn::get_subpath() {
    return _url_subpath;
}

int srt_conn::read(unsigned char* data, int len) {
    int ret = 0;

    ret = srt_recv(_conn_fd, (char*)data, len);
    if (ret <= 0) {
        srt_log_error("srt read error:%d, socket fd:%d", ret, _conn_fd);
        return ret;
    }
    return ret;
}

int srt_conn::write(unsigned char* data, int len) {
    int ret = 0;

    ret = srt_send(_conn_fd, (char*)data, len);
    if (ret <= 0) {
        srt_log_error("srt write error:%d, socket fd:%d", ret, _conn_fd);
        write_fail_cnt_++;
        return ret;
    }
    write_fail_cnt_ = 0;
    return ret;
}

int srt_conn::get_write_fail_count() {
    return write_fail_cnt_;
}