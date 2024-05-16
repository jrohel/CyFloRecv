CyFlowRec
==========

CyFlowRec is software for collecting FCS files received via the serial port (RS232 serial interface) from a Partec flow cytometer sent via the TRANSFER function.

It is intended as an open source replacement for Partec's FloTrans software.
According to the documentation, the original FloTrans software (which I have) requires a computer running Windows 95/98. 
I only tested FloTrans under Windows 10 with a USB to serial port converter (Prolific PL2303GT). FloTrans was unable to receive data. CyFlowRec on the same hardware configuration works correctly (I used gcc and Cygwin to build CyFlowRec).
Another problem with the FloTrans software is the hardcoded COM1 serial interface. This means that multiple instances cannot be run in parallel to connect multiple cytometers.

The idea is that CyFlowRec will be more configurable. And it will work under different operating systems and on different hardware architectures.
It will be usable on a workstations, but also on embedded devices. For example, in a WiFi router that can collect FCS files from connected cytometers, store them on a USB drive and share them on the network (via NFS, CIFS, ...) with other computers.

Flow Cytometer Requirements
---------------------------
The manual for Partec's FloTras software says: "The flow cytometer must be running the Partec CA3 software, version 1.319 (11/2000) or higher (Partec PA, CCA, SSC)."
CyFlowRec is written from scratch. I do not have the FloTrans source code or full documentation of the data being transferred.
For testing I have only Partec CA3 version 1.322 (1/2001).

Why am I doing this?
--------------------
It started with trying to get an old Partec PA flow cytometer working in a university lab.
The device had not been used for a long time due to a malfunction. The hard drive and probably the IDE controller had died.
I am trying to run the system from a floppy drive and transfer the measured data through the serial port.