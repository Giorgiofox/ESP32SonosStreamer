idf_component_register(SRCS "webui.c"
                       INCLUDE_DIRS "include"
                       EMBED_TXTFILES "index.html"
                       REQUIRES esp_http_server sonos audio_engine net_wifi)
