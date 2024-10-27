// Copyright (c) 2024 Winlin
//
// SPDX-License-Identifier: MIT
package main

import (
	"context"
	"encoding/json"
	"io"
	"net/http"
	"srs-proxy/errors"
	"srs-proxy/logger"
	"strings"
)

type SrsClient struct {
	Id        string  `json:"id"`
	Vhost     string  `json:"vhost"`
	Stream    string  `json:"stream"`
	Ip        string  `json:"ip"`
	PageUrl   string  `json:"pageUrl"`
	SwfUrl    string  `json:"swfUrl"`
	TcUrl     string  `json:"tcUrl"`
	Url       string  `json:"url"`
	Name      string  `json:"name"`
	Type      string  `json:"type"`
	Publish   bool    `json:"publish"`
	Alive     float32 `json:"alive"`
	SendBytes int     `json:"send_bytes"`
	RecvBytes int     `json:"recv_bytes"`
}

type SrsApiCodeResponse struct {
	Code int `json:"code"`
}

type SrsAPICommonResponse struct {
	SrsApiCodeResponse
	Server  string `json:"server"`
	Service string `json:"service"`
	Pid     string `json:"pid"`
}

type SrsClientResponse struct {
	SrsAPICommonResponse
	Client SrsClient `json:"client"`
}

type SrsClientsResponse struct {
	SrsAPICommonResponse
	Clients []SrsClient `json:"clients"`
}

type SrsKbps struct {
	Recv_30s uint32 `json:"recv_30s"`
	Send_30s uint32 `json:"send_30s"`
}

type SrsPublish struct {
	Active bool   `json:"active"`
	Cid    string `json:"cid"`
}

type SrsVideo struct {
	Codec   string `json:"codec"`
	Profile string `json:"profile"`
	Level   string `json:"level"`
	Width   uint32 `json:"width"`
	Height  uint32 `json:"height"`
}

type SrsAudio struct {
	Codec       string `json:"codec"`
	Sample_rate uint32 `json:"sample_rate"`
	Channel     uint8  `json:"channel"`
	Profile     string `json:"profile"`
}

type SrsStream struct {
	Id         string     `json:"id"`
	Name       string     `json:"name"`
	Vhost      string     `json:"vhost"`
	App        string     `json:"app"`
	TcUrl      string     `json:"tcUrl"`
	Url        string     `json:"url"`
	Live_ms    uint64     `json:"live_ms"`
	Clients    uint32     `json:"clients"`
	Frames     uint32     `json:"frames"`
	Send_bytes uint32     `json:"send_bytes"`
	Recv_bytes uint32     `json:"recv_bytes"`
	Kbps       SrsKbps    `json:"kbps"`
	Publish    SrsPublish `json:"publish"`
	Video      SrsVideo   `json:"video"`
	Audio      SrsAudio   `json:"audio"`
}

type SrsStreamResponse struct {
	SrsAPICommonResponse
	Stream SrsStream `json:"stream"`
}

type SrsStreamsResponse struct {
	SrsAPICommonResponse
	Streams []SrsStream `json:"streams"`
}

type SrsApiProxy struct {
}

func (v *SrsApiProxy) proxySrsAPI(ctx context.Context, servers []*SRSServer, w http.ResponseWriter, r *http.Request) error {
	if strings.HasPrefix(r.URL.Path, "/api/v1/clients") {
		return proxySrsClientsAPI(ctx, servers, w, r)
	} else if strings.HasPrefix(r.URL.Path, "/api/v1/streams") {
		return proxySrsStreamsAPI(ctx, servers, w, r)
	}
	return nil
}

// handle srs clients api /api/v1/clients
func proxySrsClientsAPI(ctx context.Context, servers []*SRSServer, w http.ResponseWriter, r *http.Request) error {
	defer r.Body.Close()

	clientId := ""
	if strings.HasPrefix(r.URL.Path, "/api/v1/clients/") {
		clientId = r.URL.Path[len("/api/v1/clients/"):]
	}
	logger.Df(ctx, "%v %v clientId=%v", r.Method, r.URL.Path, clientId)

	body, err := io.ReadAll(r.Body)
	if err != nil {
		apiError(ctx, w, r, err, http.StatusInternalServerError)
		return errors.Wrapf(err, "read request body err")
	}

	switch r.Method {
	case http.MethodDelete:
		for _, server := range servers {
			if ret, err := server.ApiRequest(ctx, r, body); err == nil {
				logger.Df(ctx, "response %v", string(ret))
				var res SrsApiCodeResponse
				if err := json.Unmarshal(ret, &res); err == nil && res.Code == 0 {
					apiResponse(ctx, w, r, res)
					return nil
				}
			}
		}

		err := errors.Errorf("clientId %v not found in server", clientId)
		apiError(ctx, w, r, err, http.StatusNotFound)
		return err
	case http.MethodGet:
		if len(clientId) > 0 {
			for _, server := range servers {
				var client SrsClientResponse
				if ret, err := server.ApiRequest(ctx, r, body); err == nil {
					if err := json.Unmarshal(ret, &client); err == nil && client.Code == 0 {
						apiResponse(ctx, w, r, client)
						return nil
					}
				}
			}
		} else { // get all clients
			var clients SrsClientsResponse
			for _, server := range servers {
				var res SrsClientsResponse
				if ret, err := server.ApiRequest(ctx, r, body); err == nil {
					if err := json.Unmarshal(ret, &res); err == nil && res.Code == 0 {
						clients.Clients = append(clients.Clients, res.Clients...)
					}
				}
			}

			apiResponse(ctx, w, r, clients)
			return nil
		}
	default:
		logger.Df(ctx, "/api/v1/clients %v", r.Method)
	}
	return nil
}

func proxySrsStreamsAPI(ctx context.Context, servers []*SRSServer, w http.ResponseWriter, r *http.Request) error {
	defer r.Body.Close()

	streamId := ""
	if strings.HasPrefix(r.URL.Path, "/api/v1/streams/") {
		streamId = r.URL.Path[len("/api/v1/streams/"):]
	}
	logger.Df(ctx, "%v %v streamId=%v", r.Method, r.URL.Path, streamId)

	body, err := io.ReadAll(r.Body)
	if err != nil {
		apiError(ctx, w, r, err, http.StatusInternalServerError)
		return errors.Wrapf(err, "read request body err")
	}
	if r.Method != http.MethodGet {
		err := errors.Errorf("Unsupported http method type %v", r.Method)
		apiError(ctx, w, r, err, http.StatusBadRequest)
		return err
	}
	if len(streamId) > 0 {
		var stream SrsStreamResponse
		for _, server := range servers {
			if ret, err := server.ApiRequest(ctx, r, body); err == nil {
				if err := json.Unmarshal(ret, &stream); err == nil && stream.Code == 0 {
					apiResponse(ctx, w, r, stream)
					return nil
				}
			}
		}
		ret := SrsApiCodeResponse{
			Code: 2048,
		}
		apiResponse(ctx, w, r, ret)
		return nil
	} else {
		var streams SrsStreamsResponse
		for _, server := range servers {
			var res SrsStreamsResponse
			if ret, err := server.ApiRequest(ctx, r, body); err == nil {
				if err := json.Unmarshal(ret, &res); err == nil && res.Code == 0 {
					streams.Streams = append(streams.Streams, res.Streams...)
				}
			}
		}

		apiResponse(ctx, w, r, streams)
		return nil
	}

	return nil
}
