cmake_minimum_required(VERSION 3.16.0)

if (CONFIG_IPC_SERVICE_BACKEND_UART)
target_sources(app PRIVATE "zephyr,uart-ipc-service-backend.c")
endif() # CONFIG_IPC_SERVICE_BACKEND_UART