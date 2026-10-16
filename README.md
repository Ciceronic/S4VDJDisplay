# S4VDJDisplay
VirtualDJ display support for Native Instruments Traktor Kontrol S4 MK3

Native display support for the **Native Instruments Traktor Kontrol S4 MK3** when using **VirtualDJ** on Windows.

This project enables waveform visualization on the controller's two built-in displays.

## Features

- Waveforms generated from VirtualDJ's waveform cache
- Support for decks A, B, C and D
- Deck-specific waveform colors
- Native C++ implementation
- Windows installer with automatic WinUSB driver setup
- Uninstaller that attempts to restore the original Native Instruments display driver

## Requirements

- Native Instruments Traktor Kontrol S4 MK3
- VirtualDJ 2026
- Windows 64-bit

## Installation

Download the latest installer from the GitHub Releases section (when available).

Close VirtualDJ before installation and run the installer with administrator privileges.

The installer replaces the driver for the S4 MK3 display interface (MI_04) with WinUSB. Other controller interfaces are not intended to be modified.

## Uninstallation

Use Windows' normal application uninstall procedure. The uninstaller attempts to restore the original Native Instruments display driver, which must already be available on the system.

## Project status

Tested successfully on the developer's S4 MK3, including installation, waveform display, uninstallation and driver restoration after reboot.

## Disclaimer

This is an independent community project and is not affiliated with or endorsed by Native Instruments or Atomix Productions (VirtualDJ).
