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

type SrsClientResponse struct {
	Code    int       `json:"code"`
	Server  string    `json:"server"`
	Service string    `json:"service"`
	Pid     string    `json:"pid"`
	Client  SrsClient `json:"client"`
}

type SrsClientsResponse struct {
	Code    int         `json:"code"`
	Server  string      `json:"server"`
	Service string      `json:"service"`
	Pid     string      `json:"pid"`
	Clients []SrsClient `json:"clients"`
}

type SrsClientDeleteResponse struct {
	Code int `json:"code"`
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
				var res SrsClientDeleteResponse
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
	return nil
}
