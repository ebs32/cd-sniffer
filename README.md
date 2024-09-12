# cd-sniffer

This repository contains the code for an ESP8266 SDK based project for sniffing the traffic between the CPU of a HiFi System and the CD controller board. The system was brought to me with issues when playing CDs.

The HiFi System is an LG FFH-261 and I couldn't find much information (service manual, schematics, etc.) online about it. However, it seems a similar model was sold in Brazil as Toshiba Mini System MS-6234.

The outcome of this project is to get an understanding on how the CD technology works as well as mastering the skills writing code for MCUs. If I could get the CD working that is a bonus, of course.

## Repository Organization

The layout of this repository is based on the SDK of the ESP8266 MCU. The `logs` directory contains some logs of the traffic that I have already captured with the sniffer.

## Technical Information

### Communication

The CPU uses 2 separate data channels for communicating with the CD controller board. One of the data channels, the MICOM interface, is used for sending commands from the CPU to both the [SERVO IC](docs/KB9223.PDF) and the [DSP IC](docs/KS9286B.PDF). The other data channel is used for transferring the data of the channel Q from the DSP to the CPU.

Following are some of the lines available in the CD controller board interface:

  - A/D+5V
  - XRST
  - XLT
  - SCOR
  - TRCNT
  - CLK
  - DATA
  - MUTE
  - SUBQ
  - SQCK
  - SENS
  - GFS
  - FOK

The `A/D+5V` line is an analog line shared between different switches. However, for our purposes, we are just interested in the line that indicates when the optical pickup hits the limit switch in order to stop the slide motor.

The `XRST` line is connected to the `MUTE` pin of the [DRIVER IC](docs/KA9258D.PDF), the `RESET` pin of the SERVO IC and the `RESET` pin of the DSP IC. It has been observed that the signal is low when there is no activity and high during normal operation. In contrast, the signal in the `MUTE` line is high when there is no activity and low during normal operation.

The `XLT`, `CLK` and `DATA` lines are used for the MICOM interface. The protocol is described in both the SERVO IC data sheet and the DSP IC data sheet. The logs captured belong to this channel. The `CLK` line is used for transmitting the clock signal and it is provided by the CPU. The `XLT` line is used for the latch signal to indicate the end of the transmission of a command.

The `SCOR`, `SQCK` and `SUBQ` lines channel are used for transferring the data of the channel Q to the CPU. The protocol is described in the DSP IC data sheet. As in the MICOM interface, the `SQCK` line is used for transmitting the clock signal and it is provided by the CPU. On the other hand, my guess is that the `GFS` line is used for signaling to the CPU when a frame has been locked, so the CPU can fetch the data of the Q channel. The data format of the channel Q is described in IEC 60908, also known as the Red Book.

The `SENS` line is connected to the `ISTAT` pin of both ICs and it is used for checking the status of a command sent to any of them. Given that it is a shared line between both ICs, it is clear that a command sent to an IC has to be completed before sending another one to either the same IC or the other IC. My guess is that when a command is sent this signal is set to low and once the command is completed the signal is set to high.

The `FOK` line is connected to the `FOK` pin of the SERVO IC and, according to the data sheet, it signals when the difference of the `RFO` signal and the DC coupled signal `IRF` is above a predefined voltage. It has been observed that the signal is high when there is focus while playing. In addition, according to the data sheet of the IC S1L9224X01, also from the same manufacturer of both ICs, this signal together with the `GFS` signal should be considered to determine wether a CD is being played or not.

Finally, the `TRCNT` line is connected to the `TRCNT` pin of both ICs. The signal is outputted from the SERVO IC and it seems to be used for adjusting the balance and the gain of the tracking error. I found a good explanation in the data sheet of the IC S1L9224X01, though it may work slightly different in the SERVO IC.
