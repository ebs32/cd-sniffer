# cd-sniffer

This repository contains the code for an ESP8266 SDK based project for sniffing the traffic between the CPU of a HiFi System and the CD controller board. The system was brought to me with issues when playing CDs.

The HiFi System is an LG FFH-261 and I couldn't find much information (service manual, schematics, etc.) online about it. However, it seems a similar model was sold in Brazil as Toshiba Mini System MS-6234.

The outcome of this project is to get an understanding on how the CD technology works as well as mastering the skills writing code for MCUs. If I could get the CD working that is a bonus, of course.

## Repository Organization

The layout of this repository is based on the SDK of the ESP8266 MCU. The `logs` directory contains some logs of the traffic that I have already captured with the sniffer.
