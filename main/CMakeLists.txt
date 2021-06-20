idf_component_register(SRCS "main.c" "http_server.c" "SPI_task.c"
                    INCLUDE_DIRS "."
                    EMBED_TXTFILES "certs/cacert.pem"
                                   "certs/prvtkey.pem")