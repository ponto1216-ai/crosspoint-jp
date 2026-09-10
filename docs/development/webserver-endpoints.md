# Webserver API

[Contributor guide](../contributing/README.md) · [User transfer guide](../wifi-transfer-ja.md)

Checked against v0.7.3 route registration in [CrossPointWebServer.cpp](../../src/network/CrossPointWebServer.cpp), 2026-09-10. Use the address displayed by the device during file transfer. This HTTP service has no authentication; use a trusted network.

## Routes and handlers

This is an implementation index, not a stable external API contract. Multipart upload is described below.

| Method | Path | Handler |
|---|---|---|
| GET | `/` | `handleRoot` |
| GET | `/files` | `handleFileList` |
| GET | `/js/jszip.min.js` | `handleJszip` |
| GET | `/js/aozora-epub.js` | `handleAozoraEpubJs` |
| GET | `/api/status` | `handleStatus` |
| GET | `/api/files` | `handleFileListData` |
| GET | `/download` | `handleDownload` |
| POST | `/upload` | `handleUploadPost` |
| POST | `/mkdir` | `handleCreateFolder` |
| POST | `/rename` | `handleRename` |
| POST | `/move` | `handleMove` |
| POST | `/delete` | `handleDelete` |
| GET | `/settings` | `handleSettingsPage` |
| GET | `/api/settings` | `handleGetSettings` |
| POST | `/api/settings` | `handlePostSettings` |
| GET | `/fonts` | `handleFontsPage` |
| GET | `/api/fonts` | `handleFontList` |
| GET | `/api/fonts/uploaded` | `handleFontUploaded` |
| POST | `/api/fonts/delete` | `handleFontDelete` |
| GET | `/sleep` | `handleSleepPage` |
| GET | `/api/sleep/images` | `handleSleepImageList` |
| GET | `/api/sleep/thumbnail` | `handleSleepThumbnail` |
| POST | `/api/sleep/delete` | `handleSleepDelete` |
| GET | `/api/wifi/scan` | `handleWifiScan` |
| POST | `/api/wifi/save` | `handleWifiSave` |
| GET | `/api/wifi/list` | `handleWifiList` |
| POST | `/api/wifi/delete` | `handleWifiDelete` |

`POST /upload` uses multipart form data with `handleUpload` and `handleUploadPost`. Optional `path` selects the directory. JavaScript routes serve browser assets.

## File request parameters

| Endpoint | Parameters |
|---|---|
| GET /api/files | Optional `path` directory |
| GET /download | `path` file |
| POST /mkdir | `name`, optional parent `path` |
| POST /rename | `path` source, `name` new basename |
| POST /move | `path` source, `dest` destination |
| POST /delete | `paths`: JSON array encoded as a form argument |

`/delete` does not take a single `path`. Callers must confirm the selection and inspect responses for failures. Consult each handler for validation, response fields and errors before integrating a client.

For a read-only request, replace the address with the one shown by the device:

```sh
curl --get --data-urlencode "path=/" http://192.168.1.102/api/files
```

## WebSocket upload

The WebSocket service uses port 81. The browser implementation and server state machine define framing, acknowledgement and failures. Consult [CrossPointWebServer.cpp](../../src/network/CrossPointWebServer.cpp) and [browser sources](../../src/network/html/) before using a historical upload example.

## Historical reference

The previous endpoint guide is in [archive](../archive/upstream/webserver-endpoints.md). It is not the current API contract.
