# Suggestion: Distributed events for Application Event Manager

## Goal
Simplify writing event manager applications that are distributed across multiple cores and/or chips. 

## Reasoning
The event manager system can often provide a good framework for creating modular applications and it works great when only using a single chip. It is however not that rare to have systems that consist of multiple chips where each chip is also interested in events from the other. Currently the solution to this problem is often to implement some proprietary modules that use some bus to transmit relevant data between chips. This solution is in my opinion not that good as it requires each application to "reinvent the wheel" as it were and makes developers have to write a lot of boilerplate code. This is especially relevant since Nordic offers several development kits that have multiple chips which are meant to interoperate.

## Requirements for a solution
A valid solution:
- allows modules to subscribe to events originating from another chip
- provides build mechanisms to make management of events that are common to several chips easy
- is properly documented

Nice-to-haves:
- Modular back-end
  - Could allow for different transmission media and protocols (e.g. UART, I2C, BT Mesh, IP, etc)
- Filters for which events should be emitted

## Proposed solution
My initial proposition for a solution is to create a module as a part of CAF that subscribes to relevant events and transmits them over the interconnect. The module should also be able to receive events from the interconnect and emit them to the application. A formal protocol for transmitting events should be established so the module could take care of things like endianness or retransmission (if desired) and ensuring that different event types are properly handled.
