idf_component_register(SRCS "main.cpp" "wifi.c" "web_server.c"
                    INCLUDE_DIRS "."
                    EMBED_FILES "../embedded/index.html" "../embedded/app.js" "../embedded/app.css"
        )
