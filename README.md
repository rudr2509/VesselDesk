# VesselDesk
VesselDesk is a lightweight, custom-built remote desktop application developed in C++ and Qt 6 for controlling and viewing a Linux desktop remotely over a network. It is designed as an open-source alternative concept to tools such as AnyDesk and TeamViewer, with a focus on learning, performance.
# 🚀 VesselDesk

### Custom Linux Remote Desktop Application

VesselDesk is a lightweight remote desktop application developed from scratch using **C++17, Qt 6, X11, XTest, TCP sockets, JPEG compression, and Tailscale**.

The purpose of VesselDesk is to provide a custom remote desktop system while also demonstrating how remote desktop applications work internally at the networking, screen-capture, compression, and input-injection levels.

VesselDesk can be used over a local network or through a private **Tailscale mesh network**.

---

# 📸 Project Overview

VesselDesk consists of two machines:

```text
                TAILSCALE / LAN
                     │
                     │
          ┌──────────▼──────────┐
          │                     │
          │    HOST MACHINE     │
          │                     │
          │     X11 Desktop     │
          │          │          │
          │   Screen Capture    │
          │          │          │
          │ JPEG Compression    │
          │          │          │
          │    TCP Server       │
          │      Port 5900      │
          │                     │
          └──────────┬──────────┘
                     │
                     │ Screen Frames
                     │
                     │ Keyboard / Mouse
                     │
          ┌──────────▼──────────┐
          │                     │
          │   CLIENT MACHINE    │
          │                     │
          │    TCP Client       │
          │          │          │
          │    JPEG Decoder     │
          │          │          │
          │   Remote Canvas     │
          │          │          │
          │ Keyboard / Mouse    │
          │                     │
          └─────────────────────┘
