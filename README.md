# usbbootgui

usbbootgui is a GUI application that allows the user to use a Pi Zero or Compute Module as USB accessory.

## Folders

```
src          - GUI C source code
data         - GUI data files (e.g. .ui file and icons)
debian       - Debian packaging files
gpioexpander - pre-built binary files from the gpioexpander project to let a Pi Zero act as GPIO expander
```

## How to rebuild

### Rebuilding gpio expander payload (optional)

Note: this step is *optional*, you can also skip it and use the pre-built binaries checked into git.

The gpioexpand code should be cross-compiled on a x86 Linux computer using buildroot.
First the buildroot build dependencies need to be installed.
If the Linux distribution used is Debian or Ubuntu this can be done with the command:

```
sudo apt-get update
sudo apt-get install git-core build-essential rsync libncurses-dev unzip python bc
```

Then run the following commands to remove the pre-built binaries, download the source code from the gpioexpander github repository, and build it:

```
rm -rf gpioexpander
git clone --depth 1 https://github.com/raspberrypi/gpioexpander.git
cd gpioexpander
./build.sh
```

After the build has finished, you can remove the gpioexpand/buildroot-2017.02 folder.

```
rm -rf buildroot-2017.02
```

(Otherwise `debuild` will also include all the build directories in the source code archive at the next step.)

### Rebuilding the main usbbootgui application and .deb package

The usbbootgui application can be compiled on the target system.

Install build dependencies for Raspbian:

`sudo apt-get install devscripts debhelper dh-autoreconf libglib2.0-dev libgtk2.0-dev intltool autopoint libusb-1.0-0-dev`

To build, go to the main directory, and type:

`debuild`

After build, the .deb Debian package will be located in the parent directory
