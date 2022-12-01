# nRF Connect SDK distributed events

This repository contains a proof of concept for inter-IC events in the nRF Connect SDK. It is accomplished by implementing an IPC service backend that transmits data over UART, which the event manager proxy can use to subscribe to events across MCUs. The IPC service backend can be found in the [drivers](./drivers) directory. Devicetree bindings can be found in [dts/bindigs](./dts/bindings).

This application is a basic ping-pong application that runs on two nRF52840 DKs, where one sends a ping event and the other responds with a pong event.

To run the application first cross connect pins 1.03 and 1.04 on the two DKs (tx to rx). Then build the app with `CONFIG_PING=y` on one DK and `CONFIG_PONG=y` on the other. Open a UART monitor to observe the logs and start/reset both DKs at the same time .



