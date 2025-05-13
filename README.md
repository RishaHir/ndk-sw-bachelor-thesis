# Documentation

This is a documentation for a bachelor thesis XDP DRIVER FOR NDK PLATFORM by Richard Hyroš.

# Authorship

Due to the nature of my work, which was to integrate the driver and the user-space applications alongside already existing software, I had to include some of the necessary dependencies alongside my code. This repository is a subset of [software repository for CESNET NDK FPGA platform](https://github.com/CESNET/ndk-sw/tree/main), changed to be self contained for the purposes of this bachelor thesis. All the files should have a header telling the authorship.

The exact files which should be evaluated as my work are listed bellow in the File Structure chapter.


# File Structure
This repository contains only the bear minimum to compile and run the XDP driver and the user-space apps.
## Only files I claim full authorship over and are relevant to the bachelor thesis are:

### The XDP Linux kernel driver:
- This directly coresponds to the assignment 4; Implement the designed XDP driver for DMA controllers utilized in the NDK platform.

Integrated as a part of the NDK platform driver. Files fully authored by me are:

- Every file located in folder ./drivers/kernel/drivers/nfb/xdp/ 

```
./drivers/kernel/drivers/nfb/xdp/channel.c
./drivers/kernel/drivers/nfb/xdp/channel.h
./drivers/kernel/drivers/nfb/xdp/ctrl_xdp_common.c
./drivers/kernel/drivers/nfb/xdp/ctrl_xdp_common.h
./drivers/kernel/drivers/nfb/xdp/ctrl_xdp_pp.c
./drivers/kernel/drivers/nfb/xdp/ctrl_xdp_xsk.c
./drivers/kernel/drivers/nfb/xdp/ctrl_xdp.h
./drivers/kernel/drivers/nfb/xdp/driver.c
./drivers/kernel/drivers/nfb/xdp/driver.h
./drivers/kernel/drivers/nfb/xdp/ethdev.c
./drivers/kernel/drivers/nfb/xdp/ethdev.h
./drivers/kernel/drivers/nfb/xdp/sysfs.c
./drivers/kernel/drivers/nfb/xdp/sysfs.h
```

### The AF_XDP userspace application:
- This directly coresponds to the assignment 5; Apply the implemented XDP driver in the software tools of the NDK platform.

Integrated as a part of `ndp-tool` NDK platform application. Since this was an integration into an already existing tool, my job here was implementing my solution to fit the modular design of an already existing application, rather than coming up with a new structure. Files fully authored by me are:

- Every file located in ./tools/ndptool/xdp

```
./tools/ndptool/xdp/xdp_common.c
./tools/ndptool/xdp/xdp_common.h
./tools/ndptool/xdp/xdp_generate.c
./tools/ndptool/xdp/xdp_read.c
```

## Some of the dependencies which I do NOT claim full authorship over are:
- libnfb
- The build system
- Parts of the kernel driver, which are not in the ./drivers/kernel/drivers/nfb/xdp/ folder.
- Parts of the ndp-tool application which are not in the ./tools/ndptool/xdp folder.

# Testing environment
Due to the nature of XDP being a fairly new feature, it is possible that compatibility issues might occur when running very new or very old versions of Linux.
Both XDP as implemented in the Linux kernel and libxdp library for AF_XDP userspace applications are still under active development and adding new features. Furthermore since the XDP driver was developed as a part of the NDK platform, supported hardware is needed.

### Operating system
The dirver was tested on `Oracle Linux 9` with kernel version `5.15.0-307.178.5.el9uek.x86_64`. 

### libxdp
The driver was tested with libxdp of version `1.4.2`.

### Hardware

Network card used for testing was `NFB-200G2QL` with 2 network interfaces connected via cable for physical loopback.

### Firmware
Used firmware was `NDK_MINIMAL` with `DMA MEDUSA` IP. Firmware has 16 RX and 16 TX queues split 8/8 between two ports.

More information about firmware can be found here: [NDK-APP-Minimal application](https://github.com/CESNET/ndk-app-minimal/)


# Build Instructions

1. Install all prerequisites for NDK software.

    `sudo ./build.sh --bootstrap`

2. Install all prerequisites for XDP.

	`sudo ./build.sh --bootstrap-xdp`

2. Download and extract 3rd party code archive from the GitHub Release page.

	`./build.sh --prepare`

3. Compile library and tools to the cmake-build folder.

    `./build.sh --make-xdp`

5. Compile Linux driver.

    `cd drivers; ./configure; make; cd ..`

6. Load Linux driver manually with a parameter to enable XDP.

    `sudo insmod drivers/kernel/drivers/nfb/nfb.ko xdp_enable="yes"`

If everything works correctly you should find these messages in the system log `dmesg`: 

`nfb_xdp: attached`

`IPv6: ADDRCONF(NETDEV_CHANGE): nfb0p0: link becomes ready`

`IPv6: ADDRCONF(NETDEV_CHANGE): nfb0p1: link becomes ready`

If problems occur, rebooting the card software can help.

# Usage Instructions
If you have compiled the XDP applications and loaded the driver (see the Build Instructions), then 
everything should be ready for use.

You should now be able to run the ndp-tool-xdp applications.

Assuming you run the `NDK_MINIMAL` firmware on 2 port NIC, with 8 queues per port, and ports being connected via cable for physical loopback, the application could then be tested like this:

`sudo ./cmake-build/tools/ndp-xdp-read -i0-7` - To run the receiving application on the first port (Queues 0-7).

`sudo ./cmake-build/tools/ndp-xdp-generate -i8-15 -s256` - To run the transmitting application on the second port (Queues 8-15).

You should now see traffic measured by the applications.

# Troubleshooting 

### Cannot load the driver correctly, no RX queues:

Try to reboot firmware. 

### Cannot load the driver correctly, issues with interface naming:
If the interfaces are not named correctly, it is possible you are dealing with a feature of Linux called `Predictable Network Interface Device Names`, which ironically can rename some of the interfaces in a very unpredictablel manner. Solution is to disable this feature. Whether this is enabled and how to disable this can be different depending on the Linux version you use. 

On Oracle Linux 9 the solution was to create a file `80-net-setup-link.rules` which is responsible for the predictable naming and make it a symlink to `/dev/null`.

`sudo ln -s /dev/null /etc/udev/rules.d/80-net-setup-link.rules`


### TX not showing
Check if the ethernet interfaces are enabled in the firmware.

### Queues get stuck
The NDK platform allows for opening just the RX queues or just the TX queues. In contrast, AF_XDP operates with channels (RX/TX queue pairs).

 This causes a problem when using the NDK firmwares, because by opening the AF_XDP in the TX mode, you open the underlying DMA RX queue and TX queue both.

 It is possible to get the firmware stuck by sending packets to the RX DMA queue if this queue is opened but the packets are not processed. This is an intended feature of the NDK, becuase by opening the DMA queue you tell the card, that packets should not be dropped.

 This issue is not encountered under normal circumstances, but it can show up, if there is some kind of background process that periodically sends packets to all interfaces. This can be for an example `NetworkManager`. So check if there is a traffic on the ports when they are not used and if so, disable it.

 ### Message "xdp_do_redirect error ret: -28, maybe RX cannot keep up." in the system log.
 This message and its error code correspond to errno code ENOSPC - no space on the device, this means, that there was no space in the RX buffer of the AF_XDP user-space application. It hints at an error in the user-space application logic, because the card gets more buffers to fill, than the user-space application receives. 
 
 This error can show up when the queues get stuck. (See the Queues get stuck issue)
